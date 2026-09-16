/*
 * fork_latency.c - measures how long it takes to get from fork() to the first
 * instruction executed in the child.
 *
 * This is a plain Linux program. Run it natively for a baseline and inside a
 * Junction container to measure Junction's process-clone path.
 *
 * Three latencies are reported per iteration:
 *
 *   fork -> child first instruction   t_child  - t_before_fork
 *   fork -> parent return             t_parent - t_before_fork
 *   fork -> child reaped              t_reaped - t_before_fork
 *
 * The first is the headline number. The parent samples the TSC immediately
 * before calling fork(); the child samples it as the first thing it does and
 * publishes it through a MAP_SHARED page. The TSC is invariant and shared
 * across cores of a socket, so the two samples are directly comparable.
 *
 * Because the child's sample is taken from C (after the compiler's test of
 * fork()'s return value), it includes a compare-and-branch -- a couple of
 * nanoseconds. No overhead is subtracted from any reported number; the
 * measured cost of a back-to-back TSC read pair is printed so it can be
 * accounted for by the reader.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <pthread.h>
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

/* Published by the child to the parent. */
struct shared_slot {
  volatile uint64_t child_tsc;
  volatile uint64_t seq;
  char pad[64 - 2 * sizeof(uint64_t)];
};

static struct shared_slot *slot;

static size_t iters = 1000;
static size_t warmup = 100;
static size_t footprint_mb = 0;
static size_t extra_threads = 0;
static bool raw_clone = false;
static bool csv = false;
static int pin_cpu = -1;

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s [options]\n"
          "  -n <iters>    measured iterations (default %zu)\n"
          "  -w <iters>    warmup iterations (default %zu)\n"
          "  -m <MB>       pre-faulted private anonymous memory in the parent\n"
          "                (default 0); grows the page tables fork must copy\n"
          "  -t <n>        extra live threads in the parent (default 0)\n"
          "  -r            use raw clone(SIGCHLD) instead of libc fork()\n"
          "  -p <cpu>      pin the parent to <cpu>\n"
          "  -c            emit a CSV summary line instead of a table\n"
          "  -h            this message\n",
          prog, iters, warmup);
  exit(2);
}

/*
 * fork_once - performs one fork and fills in the three timestamps.
 *
 * Returns 0 on success, -1 if the fork failed.
 */
static int fork_once(uint64_t seq, uint64_t *t0_out, uint64_t *t_parent_out,
                     uint64_t *t_reaped_out) {
  uint64_t t0 = rdtsc_now();
  long pid;

  if (raw_clone) {
    /*
     * Bare kernel clone: no pthread_atfork handlers, no malloc lock dance,
     * no TID cache fixups. This isolates the kernel's (or Junction's) clone
     * mechanism from glibc's wrapper.
     */
    pid = syscall(SYS_clone, (unsigned long)SIGCHLD, 0UL, 0UL, 0UL, 0UL);
    if (pid == 0) {
      slot->child_tsc = rdtsc_now();
      slot->seq = seq;
      syscall(SYS_exit_group, 0);
      __builtin_unreachable();
    }
  } else {
    pid = (long)fork();
    if (pid == 0) {
      slot->child_tsc = rdtsc_now();
      slot->seq = seq;
      syscall(SYS_exit_group, 0);
      __builtin_unreachable();
    }
  }

  uint64_t t_parent = rdtsc_now();
  if (pid < 0) return -1;

  int status = 0;
  while (waitpid((pid_t)pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    perror("waitpid");
    exit(1);
  }
  uint64_t t_reaped = rdtsc_now();

  /* Wait for the child's store to land (it exited, so this is immediate). */
  while (slot->seq != seq) __asm__ __volatile__("pause" ::: "memory");

  *t0_out = t0;
  *t_parent_out = t_parent;
  *t_reaped_out = t_reaped;
  return 0;
}

static void *idle_thread(void *arg) {
  volatile int *stop = arg;
  while (!*stop) {
    sleep_ms(1);
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "n:w:m:t:p:rch")) != -1) {
    switch (opt) {
      case 'n': iters = strtoul(optarg, NULL, 0); break;
      case 'w': warmup = strtoul(optarg, NULL, 0); break;
      case 'm': footprint_mb = strtoul(optarg, NULL, 0); break;
      case 't': extra_threads = strtoul(optarg, NULL, 0); break;
      case 'p': pin_cpu = atoi(optarg); break;
      case 'r': raw_clone = true; break;
      case 'c': csv = true; break;
      default: usage(argv[0]);
    }
  }
  if (iters == 0) usage(argv[0]);

  if (pin_cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(pin_cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) < 0) perror("sched_setaffinity");
  }

  slot = mmap(NULL, sizeof(*slot), PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (slot == MAP_FAILED) {
    perror("mmap(shared slot)");
    return 1;
  }
  slot->seq = 0;

  /*
   * Optionally give the parent a private dirty footprint. Under Linux this
   * makes fork's page-table copy (and therefore fork latency) grow with the
   * number of mapped pages, which is the effect we most want to see preserved
   * by a correct implementation.
   */
  char *footprint = NULL;
  size_t footprint_bytes = footprint_mb << 20;
  if (footprint_bytes) {
    footprint = mmap(NULL, footprint_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (footprint == MAP_FAILED) {
      perror("mmap(footprint)");
      return 1;
    }
    for (size_t i = 0; i < footprint_bytes; i += 4096) footprint[i] = (char)i;
  }

  volatile int stop_threads = 0;
  pthread_t *threads = NULL;
  if (extra_threads) {
    threads = calloc(extra_threads, sizeof(*threads));
    for (size_t i = 0; i < extra_threads; i++) {
      if (pthread_create(&threads[i], NULL, idle_thread, (void *)&stop_threads))
        { perror("pthread_create"); return 1; }
    }
  }

  double overhead = timing_overhead_ns();

  uint64_t *d_child = calloc(iters, sizeof(uint64_t));
  uint64_t *d_parent = calloc(iters, sizeof(uint64_t));
  uint64_t *d_reaped = calloc(iters, sizeof(uint64_t));
  if (!d_child || !d_parent || !d_reaped) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  uint64_t seq = 1;
  for (size_t i = 0; i < warmup; i++) {
    uint64_t a, b, c;
    if (fork_once(seq++, &a, &b, &c) < 0) {
      fprintf(stderr, "fork failed during warmup: %s\n", strerror(errno));
      return 1;
    }
  }

  for (size_t i = 0; i < iters; i++) {
    uint64_t t0, tp, tr;
    if (fork_once(seq, &t0, &tp, &tr) < 0) {
      fprintf(stderr, "fork failed at iteration %zu: %s\n", i, strerror(errno));
      return 1;
    }
    d_child[i] = slot->child_tsc - t0;
    d_parent[i] = tp - t0;
    d_reaped[i] = tr - t0;
    seq++;
  }

  stop_threads = 1;
  if (threads) {
    for (size_t i = 0; i < extra_threads; i++) pthread_join(threads[i], NULL);
    free(threads);
  }

  struct stats sc = stats_compute(d_child, iters);
  struct stats sp = stats_compute(d_parent, iters);
  struct stats sr = stats_compute(d_reaped, iters);

  if (csv) {
    printf("mode,footprint_mb,threads,iters,"
           "child_min,child_p50,child_p90,child_p99,child_max,child_mean,"
           "parent_p50,reaped_p50\n");
    printf("%s,%zu,%zu,%zu,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",
           raw_clone ? "clone" : "fork", footprint_mb, extra_threads, iters,
           sc.min, sc.p50, sc.p90, sc.p99, sc.max, sc.mean, sp.p50, sr.p50);
  } else {
    printf("fork latency benchmark\n");
    printf("  mechanism        : %s\n",
           raw_clone ? "clone(SIGCHLD) raw syscall" : "fork() via libc");
    printf("  iterations       : %zu (+%zu warmup)\n", iters, warmup);
    printf("  parent footprint : %zu MB private dirty (%zu pages)\n",
           footprint_mb, footprint_bytes / 4096);
    printf("  parent threads   : %zu extra\n", extra_threads);
    printf("  TSC frequency    : %.3f GHz\n", tsc_hz() / 1e9);
    printf("  timing overhead  : %.1f ns (back-to-back RDTSCP pair)\n\n",
           overhead);
    stats_print_header();
    stats_print("fork -> child 1st instruction", sc);
    stats_print("fork -> parent return", sp);
    stats_print("fork -> child reaped", sr);
    printf("\nall values in nanoseconds\n");
  }

  if (footprint) munmap(footprint, footprint_bytes);
  munmap(slot, sizeof(*slot));
  free(d_child);
  free(d_parent);
  free(d_reaped);
  return 0;
}
