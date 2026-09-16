/*
 * timing.h - shared TSC timing + statistics helpers for the fork benchmarks.
 *
 * These benchmarks are plain Linux programs: they are meant to run both
 * natively (to establish a baseline) and inside a Junction container.
 */

#ifndef JUNCTION_BENCH_FORK_TIMING_H_
#define JUNCTION_BENCH_FORK_TIMING_H_

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Read the timestamp counter, serializing against earlier instructions.
 *
 * RDTSCP does not execute until all prior instructions have retired, which is
 * what we want on the "stop" side of a measurement. The trailing LFENCE keeps
 * later instructions from being hoisted above the read, which is what we want
 * on the "start" side. Using the same primitive on both sides keeps the bias
 * symmetric (and therefore mostly cancelling) rather than merely small.
 */
static inline uint64_t rdtsc_now(void) {
  uint32_t lo, hi, aux;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
  __asm__ __volatile__("lfence" ::: "memory");
  return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Measure TSC ticks per second against CLOCK_MONOTONIC.
 *
 * The CPU has constant_tsc/nonstop_tsc, so a single calibration is valid for
 * the life of the run and is comparable across processes.
 */
static inline double tsc_hz(void) {
  static double cached = 0.0;
  if (cached != 0.0) return cached;

  const uint64_t kCalibNs = 50ULL * 1000 * 1000; /* 50 ms */
  uint64_t ns0 = monotonic_ns();
  uint64_t tsc0 = rdtsc_now();
  uint64_t ns1;
  do {
    ns1 = monotonic_ns();
  } while (ns1 - ns0 < kCalibNs);
  uint64_t tsc1 = rdtsc_now();

  cached = (double)(tsc1 - tsc0) * 1e9 / (double)(ns1 - ns0);
  return cached;
}

static inline double tsc_to_ns(uint64_t ticks) {
  return (double)ticks * 1e9 / tsc_hz();
}

/* Cost of the measurement itself, so it can be reported (not subtracted). */
static inline double timing_overhead_ns(void) {
  const int kIters = 2000;
  uint64_t best = UINT64_MAX;
  for (int i = 0; i < kIters; i++) {
    uint64_t a = rdtsc_now();
    uint64_t b = rdtsc_now();
    if (b - a < best) best = b - a;
  }
  return tsc_to_ns(best);
}

/*
 * Sleeps for @ns, resuming after signals.
 *
 * nanosleep() returns early with EINTR whenever a signal arrives, and a
 * benchmark that treats the call as a fixed delay silently measures the wrong
 * interval when it does.
 */
static inline void sleep_ns(uint64_t ns) {
  /*
   * Driven from a deadline rather than from nanosleep's remaining-time
   * argument. A signal can cut the call short, and not every implementation
   * fills in the remainder on EINTR -- trusting it can turn a bounded sleep
   * into an unbounded one.
   */
  uint64_t deadline = monotonic_ns() + ns;
  for (;;) {
    uint64_t now = monotonic_ns();
    if (now >= deadline) return;
    uint64_t left = deadline - now;
    struct timespec req;
    req.tv_sec = (time_t)(left / 1000000000ULL);
    req.tv_nsec = (long)(left % 1000000000ULL);
    if (nanosleep(&req, NULL) == 0) return;
    if (errno != EINTR) return;
  }
}

static inline void sleep_ms(unsigned ms) {
  sleep_ns((uint64_t)ms * 1000000ULL);
}

/* ---------------------------------------------------------------------- */
/* Statistics                                                             */
/* ---------------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

struct stats {
  double min, p50, p90, p99, max, mean, stddev;
  size_t n;
};

/* Computes summary statistics; sorts @v in place. */
static inline struct stats stats_compute(uint64_t *v, size_t n) {
  struct stats s;
  memset(&s, 0, sizeof(s));
  s.n = n;
  if (n == 0) return s;

  qsort(v, n, sizeof(*v), cmp_u64);

  double sum = 0.0;
  for (size_t i = 0; i < n; i++) sum += tsc_to_ns(v[i]);
  s.mean = sum / (double)n;

  double sq = 0.0;
  for (size_t i = 0; i < n; i++) {
    double d = tsc_to_ns(v[i]) - s.mean;
    sq += d * d;
  }
  s.stddev = (n > 1) ? sqrt(sq / (double)(n - 1)) : 0.0;

  s.min = tsc_to_ns(v[0]);
  s.max = tsc_to_ns(v[n - 1]);
  s.p50 = tsc_to_ns(v[(size_t)((double)n * 0.50)  < n ? (size_t)((double)n * 0.50) : n - 1]);
  s.p90 = tsc_to_ns(v[(size_t)((double)n * 0.90)  < n ? (size_t)((double)n * 0.90) : n - 1]);
  s.p99 = tsc_to_ns(v[(size_t)((double)n * 0.99)  < n ? (size_t)((double)n * 0.99) : n - 1]);
  return s;
}

static inline void stats_print_header(void) {
  printf("%-34s %9s %9s %9s %9s %9s %9s\n", "metric", "min", "p50", "p90",
         "p99", "max", "mean");
  printf("%-34s %9s %9s %9s %9s %9s %9s\n", "----------------------------------",
         "---------", "---------", "---------", "---------", "---------",
         "---------");
}

static inline void stats_print(const char *name, struct stats s) {
  printf("%-34s %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f\n", name, s.min, s.p50,
         s.p90, s.p99, s.max, s.mean);
}

#endif /* JUNCTION_BENCH_FORK_TIMING_H_ */
