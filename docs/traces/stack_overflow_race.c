/*
 * Crashes Junction through the Caladan runtime stack allocator.
 *
 * Caladan pre-reserves a pool of uthread stacks as one MAP_SHARED mapping, so
 * every address space sees them. That reservation is the *only* thing keeping
 * stack allocation from mapping memory into a single address space. Exhaust it
 * and stack_create() falls back to a fresh mmap, which lands in whichever mm
 * the allocating core happens to be bound to.
 *
 * A uthread then runs on a stack that does not exist in its address space. The
 * fault is on the stack itself, so there is no frame to report it from.
 *
 * The ordering is what makes it deterministic:
 *
 *   1. fork() first, while the pool is still intact -- the child's address
 *      space is cloned before any overflow stack exists.
 *   2. the parent spawns enough threads to exhaust the pool. The overflow
 *      stacks are mapped into the PARENT's address space only.
 *   3. the parent's threads exit, returning those stacks to the allocator.
 *   4. the child spawns threads, is handed the recycled overflow stacks, and
 *      runs on memory its address space has never mapped.
 *
 * No LibOS knobs needed: 20000 guest threads with 64 KB stacks overflow the
 * 16384-stack pool on their own.
 *
 * Native Linux prints "child: ok" and exits 0. Junction faults today.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

/* Small pthread stacks matter: with glibc's default 8 MB the guest runs out of
 * its own address space around 1000 threads, far short of the 16384-stack pool.
 * At 64 KB it reaches 20000 and overflows the pool with no LibOS knobs. */
#define GUEST_STACK (64 * 1024)
static int parent_threads = 20000;  /* big: pushes the high-water mark up */
static int child_threads = 512;     /* draws from what the parent freed */
static int child_rounds  = 8;       /* each round is another draw */
static volatile unsigned long sink;

/* Touch a good deal of stack, so a missing mapping is certain to be hit. */
static void *worker(void *arg) {
  volatile char buf[8192];
  memset((void *)buf, (int)(long)arg, sizeof buf);
  for (size_t i = 0; i < sizeof buf; i += 512) sink += buf[i];
  return NULL;
}

static int spawn_join(int n) {
  pthread_t *t = calloc(n, sizeof *t);
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, GUEST_STACK);
  int made = 0;
  for (int i = 0; i < n; i++)
    if (pthread_create(&t[i], &at, worker, (void *)(long)i) == 0) made++;
    else break;
  for (int i = 0; i < made; i++) pthread_join(t[i], NULL);
  free(t);
  return made;
}

int main(int argc, char **argv) {
  if (argc > 1) parent_threads = atoi(argv[1]);
  if (argc > 2) child_threads = atoi(argv[2]);
  if (argc > 3) child_rounds = atoi(argv[3]);
  setvbuf(stdout, NULL, _IOLBF, 0);

  int p2c[2], c2p[2];
  if (pipe(p2c) || pipe(c2p)) return 1;

  /* 1. fork BEFORE the pool is exhausted */
  pid_t c = fork();
  if (c == 0) {
    char g;
    if (read(p2c[0], &g, 1) != 1) _exit(1);
    /* 4. the child gets the recycled overflow stacks.
     *    Repeated rounds because which stack a uthread is handed depends on
     *    per-kthread magazine state; one round hits a parent-only stack about
     *    half the time, eight makes it reliable. */
    int made = 0;
    for (int r = 0; r < child_rounds; r++) made = spawn_join(child_threads);
    printf("child: ok (%d threads x %d rounds)\n", made, child_rounds);
    if (write(c2p[1], "d", 1) != 1) _exit(1);
    _exit(0);
  }

  /* 2 + 3. exhaust the pool here, then release the stacks */
  int made = spawn_join(parent_threads);
  printf("parent: spawned and joined %d threads\n", made);

  if (write(p2c[1], "g", 1) != 1) return 1;
  char d;
  ssize_t r = read(c2p[0], &d, 1);
  int st = 0;
  waitpid(c, &st, 0);
  if (r != 1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    if (WIFSIGNALED(st))
      printf("child: KILLED by signal %d\n", WTERMSIG(st));
    else
      printf("child: FAILED (exit %d)\n", WEXITSTATUS(st));
    return 1;
  }
  return 0;
}
