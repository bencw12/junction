/* Does the double-ready assertion need fork, or just signals racing wakers?
 *
 * The fork storm reproduces BUG_ON(th->thread_ready) in Caladan's scheduler,
 * but a fork storm also exercises every multi-address-space path there is. This
 * strips the fork out: one process, never cloned, so MultipleAddressSpacesExist()
 * stays false and the MAS machinery never runs. What is left is the ingredient
 * the protocol actually cares about -- threads blocking interruptibly while
 * signals arrive and wakers fire.
 *
 * Workers block on a condition variable (a futex wait underneath). A signaller
 * sprays SIGUSR1 at them with pthread_kill, so each blocking thread races a
 * signal against its waker. A waker thread broadcasts continuously. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#define NWORKERS 8

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static atomic_int stop, ticket, woken;
static pthread_t workers[NWORKERS];

static void handler(int sig) { (void)sig; }

static void *worker(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    pthread_mutex_lock(&mu);
    int mine = atomic_load(&ticket);
    /* Blocks in a futex wait; a signal may interrupt it, a broadcast may wake
     * it, and the two can race. */
    while (atomic_load(&ticket) == mine && !atomic_load(&stop))
      pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    atomic_fetch_add(&woken, 1);
  }
  return NULL;
}

static void *waker(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    pthread_mutex_lock(&mu);
    atomic_fetch_add(&ticket, 1);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    usleep(200);
  }
  return NULL;
}

static void *signaller(void *arg) {
  (void)arg;
  while (!atomic_load(&stop))
    for (int i = 0; i < NWORKERS; i++) pthread_kill(workers[i], SIGUSR1);
  return NULL;
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 3;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;   /* no SA_RESTART: interruptions are the point */
  sigaction(SIGUSR1, &sa, NULL);

  pthread_t w, s;
  for (int i = 0; i < NWORKERS; i++) pthread_create(&workers[i], NULL, worker, NULL);
  pthread_create(&w, NULL, waker, NULL);
  pthread_create(&s, NULL, signaller, NULL);

  sleep(secs);
  atomic_store(&stop, 1);
  pthread_mutex_lock(&mu);
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);

  pthread_join(w, NULL);
  pthread_join(s, NULL);
  for (int i = 0; i < NWORKERS; i++) pthread_join(workers[i], NULL);
  printf("no fork: %d wakeups over %d s, survived\n", atomic_load(&woken), secs);
  return 0;
}
