/* Does the double-ready assertion need address-space cloning, or just process
 * churn delivering signals while threads block?
 *
 * fork() cannot answer this: it did not exist before the multi-address-space
 * work (pre-MAS DoClone returns -ENOSYS for anything but vfork or a thread), so
 * "the fork storm without MAS" is not a runnable configuration.
 *
 * vfork() is. It creates a real process -- real exit, real SIGCHLD, real
 * teardown -- while *sharing* the caller's address space, so nothing is cloned
 * and the MAS paths stay inert. Threads block in futex waits alongside, which
 * is what the interruptible-wait protocol is about.
 *
 * If the assertion reproduces here, address-space cloning is not required for
 * it and the MAS work is exonerated. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define NSPAWN 4
#define NWAIT  4

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static atomic_int stop, ticket, spawned, woken;

/* Spawns processes that exit immediately: each exit delivers SIGCHLD to this
 * process while the waiter threads below are blocked in futex waits. */
static void *spawner(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    pid_t p = vfork();
    if (p == 0) _exit(0);
    if (p < 0) { usleep(100); continue; }
    int st;
    waitpid(p, &st, 0);
    atomic_fetch_add(&spawned, 1);
  }
  return NULL;
}

static void *waiter(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    pthread_mutex_lock(&mu);
    int mine = atomic_load(&ticket);
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
    usleep(150);
  }
  return NULL;
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 10;
  pthread_t sp[NSPAWN], wt[NWAIT], wk;

  for (int i = 0; i < NSPAWN; i++) pthread_create(&sp[i], NULL, spawner, NULL);
  for (int i = 0; i < NWAIT; i++) pthread_create(&wt[i], NULL, waiter, NULL);
  pthread_create(&wk, NULL, waker, NULL);

  sleep(secs);
  atomic_store(&stop, 1);
  pthread_mutex_lock(&mu);
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);

  for (int i = 0; i < NSPAWN; i++) pthread_join(sp[i], NULL);
  for (int i = 0; i < NWAIT; i++) pthread_join(wt[i], NULL);
  pthread_join(wk, NULL);
  printf("vfork storm: %d processes, %d wakeups over %d s, survived\n",
         atomic_load(&spawned), atomic_load(&woken), secs);
  return 0;
}
