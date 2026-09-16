/* Does a hole punch by one mm invalidate another mm's TLB entries?
 *
 * Child touches the page on CPU B (populating its PTE and TLB entry), then
 * spins reading it. Parent, on CPU A, punches the hole. If the shootdown
 * reaches the child's mm, the child sees zeros. If the child's TLB entry were
 * stale, it would keep reading the old bytes forever. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/memfd.h>
#include <linux/falloc.h>
#include <time.h>

#define SZ 4096

static void pin(int cpu) {
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
  sched_setaffinity(0, sizeof s, &s);
}
static double now(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
  int fd = memfd_create("t", 0);
  ftruncate(fd, SZ);
  volatile char *p = mmap(NULL, SZ, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  memset((void *)p, 'A', SZ);

  int p2c[2], c2p[2];
  if (pipe(p2c) || pipe(c2p)) return 1;

  pid_t c = fork();
  if (c == 0) {
    pin(3);
    /* populate PTE + TLB in THIS mm, on this CPU */
    volatile char x = p[0];
    (void)x;
    if (write(c2p[1], "r", 1) != 1) _exit(1);

    double t0 = now();
    /* spin without any syscall, so nothing forces a TLB refill */
    while (p[0] == 'A') {
      if (now() - t0 > 3.0) {
        printf("child : STILL reading 'A' after 3s -> TLB entry was NOT shot down\n");
        fflush(stdout);
        _exit(2);
      }
    }
    printf("child : observed 0x%02x after %.1f us -> shootdown reached this mm\n",
           p[0], (now() - t0) * 1e6);
    fflush(stdout);
    _exit(0);
  }

  pin(1);
  char r; if (read(c2p[0], &r, 1) != 1) return 1;
  printf("parent: child has touched the page on another CPU; punching hole\n");
  if (fallocate(fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE, 0, SZ))
    perror("fallocate");
  int st = 0; waitpid(c, &st, 0);
  printf("parent: child exit=%d\n", WEXITSTATUS(st));
  return WEXITSTATUS(st);
}
