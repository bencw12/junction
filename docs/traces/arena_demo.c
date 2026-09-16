/* Why "strict 1:1" matters: a pointer stored inside the memfd is only
 * meaningful to another address space if the address determines the offset.
 *
 * Arena rule:  offset = addr - BASE
 * Fault rule:  on a fault at A, map the memfd at offset (A - BASE).
 *
 * The child's fault handler below IS the whole propagation mechanism -- no log,
 * no epochs. It works right up until someone mremaps. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/memfd.h>
#include <linux/falloc.h>
#include <fcntl.h>

#define BASE   0x530000000000UL
#define ARENA  (1UL << 30)

static int fd;

struct node { unsigned long value; struct node *next; };

/* The lazy-propagation fault handler, in full. */
static void on_fault(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)uc;
  unsigned long a = (unsigned long)si->si_addr & ~4095UL;
  mmap((void *)a, 4096, PROT_READ | PROT_WRITE,
       MAP_SHARED | MAP_FIXED, fd, a - BASE);      /* <-- the 1:1 rule */
}

static void install_handler(void) {
  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}

/* Allocate at a chosen arena address, honouring the 1:1 rule. */
static void *arena_map(unsigned long addr) {
  void *p = mmap((void *)addr, 4096, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED, fd, addr - BASE);
  if (p == MAP_FAILED) { perror("arena_map"); exit(1); }
  return p;
}

int main(int argc, char **argv) {
  int use_copy = argc > 1 && !strcmp(argv[1], "copy");
  fd = memfd_create("arena", 0);
  if (fd < 0 || ftruncate(fd, ARENA)) { perror("memfd"); return 1; }

  struct node *n1 = arena_map(BASE + 0x1000);   /* offset 0x1000 */
  struct node *n2 = arena_map(BASE + 0x2000);   /* offset 0x2000 */
  n1->value = 42; n1->next = NULL;
  n2->value = 99; n2->next = n1;                /* a POINTER, stored in the memfd */

  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("parent: n2 at %p -> next=%p (value %lu)\n",
         (void *)n2, (void *)n2->next, n2->next->value);

  int p2c[2], c2p[2];
  if (pipe(p2c) || pipe(c2p)) return 1;

  pid_t c = fork();
  if (c == 0) {
    /* Child: its own mm. It has n1 and n2 inherited, but we unmap them so it
     * must fault them back in through the 1:1 rule, exactly as a freshly
     * cloned address space would. */
    munmap((void *)(BASE + 0x1000), 4096);
    munmap((void *)(BASE + 0x2000), 4096);
    install_handler();
    char g; if (read(p2c[0], &g, 1) != 1) _exit(1);

    struct node *m2 = (struct node *)(BASE + 0x2000);
    printf("child : n2 -> next=%p\n", (void *)m2->next);
    printf("child : following that pointer gives value %lu   %s\n",
           m2->next->value,
           m2->next->value == 42 ? "<- correct" : "<- WRONG (should be 42)");
    fflush(stdout);
    if (write(c2p[1], "d", 1) != 1) _exit(1);
    _exit(m2->next->value == 42 ? 0 : 1);
  }

  void *moved;
  if (!use_copy) {
    /* WRONG: grow the way realloc does. mremap moves the VMA and leaves the
     * bytes at offset 0x1000, breaking the arithmetic in this mm only. */
    moved = mremap(n1, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED,
                   (void *)(BASE + 0x5000));
    if (moved == MAP_FAILED) { perror("mremap"); return 1; }
    printf("parent: [MOVE] mremapped n1 to %p\n", moved);
    printf("        in this mm that address is offset 0x1000; the rule says 0x5000\n");
  } else {
    /* RIGHT: allocate a new range (whose offset matches its address), copy the
     * bytes inside the memfd, punch the old hole. Nothing moves. */
    moved = arena_map(BASE + 0x5000);           /* offset 0x5000 */
    memcpy(moved, n1, sizeof(struct node));     /* copy within the memfd */
    fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0x1000, 4096);
    printf("parent: [COPY] allocated 0x%lx (offset 0x5000), copied, punched 0x1000\n",
           BASE + 0x5000);
    printf("        address and offset still agree\n");
  }
  n2->next = (struct node *)moved;              /* update the shared pointer */
  printf("parent: reads value %lu through the new pointer\n\n", n2->next->value);

  if (write(p2c[1], "g", 1) != 1) return 1;
  char d; if (read(c2p[0], &d, 1) != 1) return 1;
  int st = 0; waitpid(c, &st, 0);
  return WEXITSTATUS(st);
}
