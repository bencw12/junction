/*
 * Crashes Junction through its own glibc heap.
 *
 * Junction pre-grows its heap by 64 MB at init (InitAddressSpaces) and the
 * address-space sweep converts that region to MAP_SHARED, so every address
 * space sees it. Past the reserve, glibc cannot use brk -- the seccomp handler
 * forces it to 0 -- so it falls back to mmap, and the SIGSYS handler executes
 * that mmap natively into whichever address space the calling core is bound
 * to.
 *
 * That memory then goes into glibc's free lists, which are *shared* LibOS
 * state. A later LibOS allocation on behalf of a different process is handed a
 * chunk that only one address space has mapped.
 *
 * The ordering:
 *   1. fork() first, so the child's address space is cloned before the heap
 *      has grown;
 *   2. the parent drives LibOS allocation past the reserve -- each guest mmap
 *      costs a VMArea node in the process's interval tree -- so glibc mmaps a
 *      new heap region into the PARENT's address space;
 *   3. the parent unmaps, returning those nodes to glibc's free lists;
 *   4. the child allocates, is handed a chunk from the parent-only region, and
 *      Junction writes a VMArea into memory the child's address space has
 *      never had.
 *
 * Gaps between the mappings stop Junction merging adjacent VMAs, so each one
 * really does cost a node.
 *
 * Run with a shrunken reserve so step 2 does not need 64 MB:
 *   JUNCTION_DEBUG_HEAP_RESERVE_MB=1
 *
 * Native Linux prints "child: ok" and exits 0.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE 4096

/*
 * Make Junction hold n VMArea nodes.
 *
 * Consecutive mmap()s with a NULL hint get adjacent addresses from
 * FindFreeRange and Junction merges them, so a loop of one-page mmaps costs one
 * node, not n. Instead map one region and mprotect alternate pages: each
 * protection boundary forces a split, so n pages become ~n VMAs that cannot be
 * merged back.
 */
static int churn(int n, const char *who) {
  size_t len = (size_t)n * PAGE;
  char *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) return 0;

  int splits = 0;
  for (int i = 0; i < n; i += 2) {
    if (mprotect(base + (size_t)i * PAGE, PAGE, PROT_READ) == 0) splits++;
  }
  munmap(base, len);
  (void)who;
  return splits;
}

int main(int argc, char **argv) {
  int parent_maps = argc > 1 ? atoi(argv[1]) : 200000;
  int child_maps  = argc > 2 ? atoi(argv[2]) : 20000;
  int rounds      = argc > 3 ? atoi(argv[3]) : 4;
  setvbuf(stdout, NULL, _IOLBF, 0);

  int p2c[2], c2p[2];
  if (pipe(p2c) || pipe(c2p)) return 1;

  pid_t c = fork();                    /* 1. before the heap grows */
  if (c == 0) {
    char g;
    if (read(p2c[0], &g, 1) != 1) _exit(1);
    int made = 0;
    for (int r = 0; r < rounds; r++) made = churn(child_maps, "child");
    printf("child: ok (%d splits x %d rounds)\n", made, rounds);
    if (write(c2p[1], "d", 1) != 1) _exit(1);
    _exit(0);
  }

  int made = churn(parent_maps, "parent");   /* 2 + 3 */
  printf("parent: created %d VMA splits\n", made);

  if (write(p2c[1], "g", 1) != 1) return 1;
  char d;
  ssize_t r = read(c2p[0], &d, 1);
  int st = 0;
  waitpid(c, &st, 0);
  if (r != 1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    if (WIFSIGNALED(st)) printf("child: KILLED by signal %d\n", WTERMSIG(st));
    else printf("child: FAILED (exit %d)\n", WEXITSTATUS(st));
    return 1;
  }
  return 0;
}
