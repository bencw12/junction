/*
 * Crashes Junction from a pure guest program -- no LibOS knobs, no forcing.
 *
 * Every pipe Junction creates allocates a 64 KB buffer from the LibOS's own
 * glibc heap:
 *
 *     inline constexpr size_t kPipeSize = 16 * kPageSize;   // limits.h
 *     auto pipe = std::make_shared<StreamPipe>(kPipeSize);  // fs/pipe.cc
 *     std::vector<std::byte> buf_;                          // byte_channel.h
 *
 * so ~1000 concurrent pipes is the entire 64 MB heap reserve that
 * InitAddressSpaces() pre-grows and the address-space sweep makes shared. Past
 * it glibc cannot use brk (the seccomp handler forces it to 0) and falls back
 * to mmap, which the SIGSYS handler runs natively into whichever address space
 * the calling core is bound to.
 *
 * glibc's free lists are shared LibOS state, so those chunks are then handed to
 * LibOS code running under a *different* address space.
 *
 *   1. fork() first, so the child's address space is cloned before the heap
 *      has grown;
 *   2. the parent opens enough pipes to push the heap past the reserve. The
 *      new heap region is mapped into the PARENT's address space only;
 *   3. the parent closes them, returning the 64 KB buffers to glibc;
 *   4. the child opens pipes and Junction writes a pipe buffer into memory the
 *      child's address space has never had.
 *
 * Native Linux prints "child: ok" and exits 0.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>

static int open_pipes(int want, int (*fds)[2]) {
  int made = 0;
  for (int i = 0; i < want; i++) {
    if (pipe(fds[made]) != 0) break;
    /* Touch it, so the buffer is really used rather than lazily ignored. */
    char c = 'x';
    if (write(fds[made][1], &c, 1) != 1) { close(fds[made][0]); close(fds[made][1]); break; }
    if (read(fds[made][0], &c, 1) != 1)  { close(fds[made][0]); close(fds[made][1]); break; }
    made++;
  }
  return made;
}

static void close_pipes(int n, int (*fds)[2]) {
  for (int i = 0; i < n; i++) { close(fds[i][0]); close(fds[i][1]); }
}

static int churn(int want) {
  int (*fds)[2] = calloc(want, sizeof *fds);
  if (!fds) return 0;
  int made = open_pipes(want, fds);
  close_pipes(made, fds);
  free(fds);
  return made;
}

int main(int argc, char **argv) {
  int parent_pipes = argc > 1 ? atoi(argv[1]) : 3000;
  int child_pipes  = argc > 2 ? atoi(argv[2]) : 500;
  int rounds       = argc > 3 ? atoi(argv[3]) : 4;
  setvbuf(stdout, NULL, _IOLBF, 0);

  struct rlimit rl = { .rlim_cur = 65536, .rlim_max = 65536 };
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  printf("nofile limit: %lu (need %d)\n", (unsigned long)rl.rlim_cur,
         parent_pipes * 2 + 16);

  int p2c[2], c2p[2];
  if (pipe(p2c) || pipe(c2p)) return 1;

  pid_t c = fork();                       /* 1. before the heap grows */
  if (c == 0) {
    char g;
    if (read(p2c[0], &g, 1) != 1) _exit(1);
    int made = 0;
    for (int r = 0; r < rounds; r++) made = churn(child_pipes);   /* 4 */
    printf("child: ok (%d pipes x %d rounds)\n", made, rounds);
    if (write(c2p[1], "d", 1) != 1) _exit(1);
    _exit(0);
  }

  int made = churn(parent_pipes);         /* 2 + 3 */
  printf("parent: opened and closed %d pipes (%d MB of pipe buffer)\n",
         made, (int)((long)made * 65536 / (1024 * 1024)));

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
