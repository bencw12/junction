/* Can the double-ready assertion be reached with threads alone -- no fork?
 *
 * Yes. Under Junction:
 *   thread_churn_storm 3 2 1      guest dies with SIGABRT within 0.1 s:
 *                                 munmap() returned -ERESTARTSYS to glibc,
 *                                 whose free_stacks() aborts on any failure
 *   thread_churn_storm 3 2 1 1    (SA_RESTART) Caladan's
 *                                 BUG_ON(th->thread_ready), 3 of 3
 * Both reproduce on the pre-fix baseline binary too. See
 * docs/bug-fork-storm-hang.md.
 *
 * The fork storm's distinguishing feature is not address-space cloning as such,
 * it is churn: thousands of short-lived children whose exits fire futex wakes
 * at threads blocked in joins, while signals arrive. Thread churn drives the
 * identical code without cloning anything:
 *
 *   thread exit -> Thread::~Thread() -> *child_tid = 0 -> FutexTable::Wake()
 *   pthread_join -> futex wait
 *
 * That is the same pair the fork storm hangs between, so if the assertion is a
 * property of the wait protocol rather than of the multi-address-space work, it
 * should be reachable here. Threads never clone an address space, so the MAS
 * paths stay inert. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static atomic_int stop, made, woken;
static int nchurn = 8, signals = 1, restart = 0;
static pthread_t churners[64];

static void handler(int s) { (void)s; }
static void *nothing(void *a) { (void)a; return NULL; }

/* Each churner continuously creates and joins a short-lived thread: create,
 * exit, futex-wake on the tid word, join. */
static void *churn(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    pthread_t t;
    if (pthread_create(&t, NULL, nothing, NULL) != 0) { usleep(50); continue; }
    pthread_join(t, NULL);
    atomic_fetch_add(&made, 1);
  }
  return NULL;
}

/* Signals racing those wakes, which is the other half of the protocol. */
static void *signaller(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    for (int i = 0; i < nchurn; i++) pthread_kill(churners[i], SIGUSR1);
    atomic_fetch_add(&woken, 1);
  }
  return NULL;
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 10;
  if (argc > 2) nchurn = atoi(argv[2]);
  if (argc > 3) signals = atoi(argv[3]);
  if (argc > 4) restart = atoi(argv[4]);   /* SA_RESTART on the handler */
  if (nchurn > 64) nchurn = 64;

  struct sigaction sa = {0};
  sa.sa_handler = handler;
  if (restart) sa.sa_flags = SA_RESTART;
  sigaction(SIGUSR1, &sa, NULL);

  for (int i = 0; i < nchurn; i++) pthread_create(&churners[i], NULL, churn, NULL);
  pthread_t s;
  if (signals) pthread_create(&s, NULL, signaller, NULL);

  sleep(secs);
  atomic_store(&stop, 1);
  for (int i = 0; i < nchurn; i++) pthread_join(churners[i], NULL);
  if (signals) pthread_join(s, NULL);

  printf("thread churn: %d threads created+joined by %d churners over %d s%s\n",
         atomic_load(&made), nchurn, secs, signals ? " (with signals)" : "");
  return 0;
}
