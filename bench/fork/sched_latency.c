/*
 * sched_latency.c - context switch latency, within and across processes.
 *
 * Multiple address spaces are not free: a scheduler that moves a core from one
 * process's thread to another's has to rebind the core to the other address
 * space. Threads of the *same* process should be unaffected, because the
 * address space does not change. This benchmark measures both so the claim can
 * be checked rather than assumed.
 *
 * Modes:
 *   yield  - N threads of one process spinning on sched_yield(); reports the
 *            cost of one scheduler round trip with no address space change.
 *   futex  - two threads of one process ping-ponging on a futex; reports the
 *            cost of a block/wake pair, again with no address space change.
 *   procs  - two *processes* ping-ponging on a futex in shared memory. Every
 *            hand-off crosses an address space boundary, so this is where the
 *            cost of multiple address spaces shows up.
 *
 * Run all three under Junction with the address-space module loaded and again
 * without it: "yield" and "futex" should be unchanged, and "procs" is only
 * available when fork() works.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "timing.h"

static long futex_wait(atomic_int *addr, int val) {
  return syscall(SYS_futex, addr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static long futex_wake(atomic_int *addr, int n) {
  return syscall(SYS_futex, addr, FUTEX_WAKE, n, NULL, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* yield: N threads of one process                                        */
/* ---------------------------------------------------------------------- */

struct yield_ctx {
  atomic_int stop;
  atomic_ullong yields;
};

static void *yield_worker(void *arg) {
  struct yield_ctx *c = arg;
  unsigned long long local = 0;
  while (!atomic_load_explicit(&c->stop, memory_order_relaxed)) {
    sched_yield();
    local++;
  }
  atomic_fetch_add(&c->yields, local);
  return NULL;
}

static void bench_yield(unsigned nthreads, unsigned ms) {
  struct yield_ctx c;
  atomic_init(&c.stop, 0);
  atomic_init(&c.yields, 0);

  pthread_t *th = calloc(nthreads, sizeof(*th));
  uint64_t t0 = monotonic_ns();
  for (unsigned i = 0; i < nthreads; i++)
    if (pthread_create(&th[i], NULL, yield_worker, &c) != 0) {
      perror("pthread_create");
      exit(1);
    }

  sleep_ms(ms);
  atomic_store(&c.stop, 1);
  for (unsigned i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
  uint64_t elapsed = monotonic_ns() - t0;

  unsigned long long n = atomic_load(&c.yields);
  printf("  yield  %2u threads : %8.0f ns per yield (%llu yields in %.2f s)\n",
         nthreads, (double)elapsed / (double)(n ? n : 1), n,
         (double)elapsed / 1e9);
  free(th);
}

/* ---------------------------------------------------------------------- */
/* futex ping-pong                                                        */
/* ---------------------------------------------------------------------- */

struct pingpong {
  atomic_int turn;    /* whose turn it is: 0 or 1 */
  atomic_int done;
  unsigned long rounds;
};

/*
 * One side of a futex ping-pong. Flips the baton to the other side, wakes it,
 * and blocks until the baton comes back.
 */
static void pingpong_side(struct pingpong *pp, int me) {
  for (unsigned long i = 0; i < pp->rounds; i++) {
    /* Wait for our turn. */
    while (atomic_load(&pp->turn) != me) {
      if (atomic_load(&pp->done)) return;
      futex_wait(&pp->turn, me ^ 1);
    }
    /* Hand the baton over. */
    atomic_store(&pp->turn, me ^ 1);
    futex_wake(&pp->turn, 1);
  }
}

static void *pingpong_thread(void *arg) {
  pingpong_side(arg, 1);
  return NULL;
}

static void bench_futex(unsigned long rounds) {
  struct pingpong *pp = mmap(NULL, sizeof(*pp), PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (pp == MAP_FAILED) { perror("mmap"); exit(1); }
  atomic_init(&pp->turn, 0);
  atomic_init(&pp->done, 0);
  pp->rounds = rounds;

  pthread_t th;
  if (pthread_create(&th, NULL, pingpong_thread, pp) != 0) {
    perror("pthread_create");
    exit(1);
  }

  uint64_t t0 = monotonic_ns();
  pingpong_side(pp, 0);
  uint64_t elapsed = monotonic_ns() - t0;

  atomic_store(&pp->done, 1);
  atomic_store(&pp->turn, 1);
  futex_wake(&pp->turn, 1);
  pthread_join(th, NULL);

  printf("  futex  same process: %8.0f ns per round trip (%lu round trips)\n",
         (double)elapsed / (double)rounds, rounds);
  munmap(pp, sizeof(*pp));
}

/* ---------------------------------------------------------------------- */
/* cross-process ping-pong                                                */
/* ---------------------------------------------------------------------- */

static void bench_procs(unsigned long rounds) {
  struct pingpong *pp = mmap(NULL, sizeof(*pp), PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (pp == MAP_FAILED) { perror("mmap"); exit(1); }
  atomic_init(&pp->turn, 0);
  atomic_init(&pp->done, 0);
  pp->rounds = rounds;

  pid_t pid = fork();
  if (pid < 0) {
    printf("  procs  cross process: unavailable (fork failed: %s)\n",
           strerror(errno));
    munmap(pp, sizeof(*pp));
    return;
  }
  if (pid == 0) {
    pingpong_side(pp, 1);
    _exit(0);
  }

  uint64_t t0 = monotonic_ns();
  pingpong_side(pp, 0);
  uint64_t elapsed = monotonic_ns() - t0;

  atomic_store(&pp->done, 1);
  atomic_store(&pp->turn, 1);
  futex_wake(&pp->turn, 1);
  int status;
  waitpid(pid, &status, 0);

  printf("  futex  cross process: %8.0f ns per round trip (%lu round trips)\n",
         (double)elapsed / (double)rounds, rounds);
  munmap(pp, sizeof(*pp));
}

/* ---------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
  unsigned long rounds = 100000;
  unsigned ms = 1000;
  bool do_yield = true, do_futex = true, do_procs = true;

  int opt;
  while ((opt = getopt(argc, argv, "r:t:m:h")) != -1) {
    switch (opt) {
      case 'r': rounds = strtoul(optarg, NULL, 0); break;
      case 't': ms = (unsigned)atoi(optarg); break;
      case 'm':
        do_yield = do_futex = do_procs = false;
        if (!strcmp(optarg, "yield")) do_yield = true;
        else if (!strcmp(optarg, "futex")) do_futex = true;
        else if (!strcmp(optarg, "procs")) do_procs = true;
        else { fprintf(stderr, "unknown mode %s\n", optarg); return 2; }
        break;
      default:
        fprintf(stderr,
                "usage: %s [-r rounds] [-t ms] [-m yield|futex|procs]\n",
                argv[0]);
        return 2;
    }
  }

  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("context switch latency\n");

  if (do_yield) {
    bench_yield(2, ms);
    bench_yield(8, ms);
  }
  if (do_futex) bench_futex(rounds);
  if (do_procs) bench_procs(rounds);
  return 0;
}
