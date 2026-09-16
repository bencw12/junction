/*
 * fork_stress.c - correctness and stress test for the process clone mechanism.
 *
 * This is a plain Linux program. Run it natively to confirm the checks encode
 * real Linux behaviour, then run it inside a Junction container to confirm
 * Junction's fork() is semantically identical.
 *
 * Part 1 ("semantics") asserts the observable behaviour that POSIX and Linux
 * promise about fork: return values and parentage, copy-on-write of every
 * private mapping class, sharing of MAP_SHARED mappings, descriptor and file
 * offset inheritance, signal state inheritance, thread collapse, atfork
 * handlers, timer reset, reparenting of orphans, and exit status propagation.
 *
 * Part 2 ("stress") hammers the mechanism: concurrent fork storms from many
 * threads, deep fork chains, wide fan-outs, and forks of processes with large
 * dirty footprints whose contents are checksummed in the child to catch any
 * address-space aliasing. It reports achieved fork throughput.
 *
 * Exit status is 0 only if every check passed.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "timing.h"

/* ---------------------------------------------------------------------- */
/* Test harness                                                           */
/* ---------------------------------------------------------------------- */

static int checks_run;
static int checks_failed;
static bool verbose;

static void vlog(const char *fmt, ...) {
  if (!verbose) return;
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
}

#define CHECK(cond, ...)                            \
  do {                                              \
    checks_run++;                                   \
    if (!(cond)) {                                  \
      checks_failed++;                              \
      printf("  FAIL: ");                           \
      printf(__VA_ARGS__);                          \
      printf("   (%s:%d)\n", __FILE__, __LINE__);   \
      fflush(stdout);                               \
    } else {                                        \
      vlog("  ok: " __VA_ARGS__);                   \
      vlog("\n");                                   \
    }                                               \
  } while (0)

static void section(const char *name) {
  printf("[ %s ]\n", name);
  fflush(stdout);
}

/*
 * Children report failures through their exit status, which the parent folds
 * back into the global counters. A child that fails a check exits with the
 * number of failures (clamped), so the parent can attribute them.
 */
#define CHILD_CHECK(cond, failcount, ...)     \
  do {                                        \
    if (!(cond)) {                            \
      (failcount)++;                          \
      printf("  FAIL(child): ");              \
      printf(__VA_ARGS__);                    \
      printf("   (%s:%d)\n", __FILE__, __LINE__); \
      fflush(stdout);                         \
    }                                         \
  } while (0)

/* Folds a child's reported failure count into the parent's totals. */
static void absorb_child(int status, const char *what) {
  checks_run++;
  if (!WIFEXITED(status)) {
    checks_failed++;
    printf("  FAIL: %s: child did not exit normally (status 0x%x)\n", what,
           status);
    return;
  }
  int n = WEXITSTATUS(status);
  if (n != 0) {
    checks_failed += n;
    printf("  FAIL: %s: child reported %d failure(s)\n", what, n);
  } else {
    vlog("  ok: %s\n", what);
  }
}

/* ---------------------------------------------------------------------- */
/* Small helpers                                                          */
/* ---------------------------------------------------------------------- */

static ssize_t xwrite(int fd, const void *buf, size_t n) {
  const char *p = buf;
  size_t done = 0;
  while (done < n) {
    ssize_t r = write(fd, p + done, n - done);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    done += (size_t)r;
  }
  return (ssize_t)done;
}

static ssize_t xread(int fd, void *buf, size_t n) {
  char *p = buf;
  size_t done = 0;
  while (done < n) {
    ssize_t r = read(fd, p + done, n - done);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) break;
    done += (size_t)r;
  }
  return (ssize_t)done;
}

static void sync_send(int fd, char c) { xwrite(fd, &c, 1); }

static char sync_recv(int fd) {
  char c = 0;
  if (xread(fd, &c, 1) != 1) return 0;
  return c;
}

static pid_t xwaitpid(pid_t pid, int *status) {
  pid_t r;
  do {
    r = waitpid(pid, status, 0);
  } while (r < 0 && errno == EINTR);
  return r;
}

static uint64_t checksum(const void *buf, size_t len) {
  /* FNV-1a over 8-byte words; good enough to catch aliasing. */
  const uint64_t *p = buf;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len / 8; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void burn_cpu_ms(unsigned ms) {
  uint64_t end = monotonic_ns() + (uint64_t)ms * 1000000ULL;
  volatile uint64_t x = 0;
  while (monotonic_ns() < end)
    for (int i = 0; i < 10000; i++) x += i;
}

/*
 * probe_unmapped - reads one byte at @addr and reports whether it faulted.
 *
 * Used to ask "is this address absent from my address space?" without relying
 * on mincore(), which not every environment implements.
 */
static sigjmp_buf probe_jmp;
static volatile sig_atomic_t probe_faulted;

static void probe_sigsegv(int signo) {
  (void)signo;
  probe_faulted = 1;
  siglongjmp(probe_jmp, 1);
}

static bool probe_unmapped(void *addr) {
  struct sigaction sa, old_segv, old_bus;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = probe_sigsegv;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, &old_segv);
  sigaction(SIGBUS, &sa, &old_bus);

  probe_faulted = 0;
  if (sigsetjmp(probe_jmp, 1) == 0) {
    volatile unsigned char v = *(volatile unsigned char *)addr;
    (void)v;
  }

  sigaction(SIGSEGV, &old_segv, NULL);
  sigaction(SIGBUS, &old_bus, NULL);
  return probe_faulted != 0;
}

/* ---------------------------------------------------------------------- */
/* Part 1: semantics                                                      */
/* ---------------------------------------------------------------------- */

static int g_data = 0x11111111;     /* .data  */
static int g_bss;                   /* .bss   */

static void test_identity(void) {
  section("identity: return values, pid, ppid");

  pid_t parent_pid = getpid();
  int pipefd[2];
  if (pipe(pipefd) < 0) { perror("pipe"); exit(1); }

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    pid_t self = getpid();
    CHILD_CHECK(getppid() == parent_pid, f,
                "child getppid()=%d, want %d", getppid(), parent_pid);
    CHILD_CHECK(self != parent_pid, f, "child pid equals parent pid (%d)", self);
    xwrite(pipefd[1], &self, sizeof(self));
    _exit(f);
  }
  CHECK(pid > 0, "fork() returned %d in parent, want > 0", pid);

  pid_t child_reported = -1;
  xread(pipefd[0], &child_reported, sizeof(child_reported));
  CHECK(child_reported == pid,
        "child's getpid()=%d but fork() returned %d to the parent",
        child_reported, pid);

  int status;
  CHECK(xwaitpid(pid, &status) == pid, "waitpid returned the forked pid");
  absorb_child(status, "identity checks in child");
  close(pipefd[0]);
  close(pipefd[1]);
}

static void test_cow(void) {
  section("copy-on-write: .data, .bss, heap, stack, MAP_PRIVATE; MAP_SHARED");

  const size_t kPriv = 256 * 1024;
  char *heap = malloc(kPriv);
  char *priv = mmap(NULL, kPriv, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  char stackbuf[4096];
  if (!heap || priv == MAP_FAILED || shared == MAP_FAILED) {
    printf("  FAIL: setup mappings (heap=%p priv=%s shared=%s errno=%s)\n",
           (void *)heap, priv == MAP_FAILED ? "FAILED" : "ok",
           shared == MAP_FAILED ? "FAILED" : "ok", strerror(errno));
    checks_failed++;
    return;
  }

  memset(heap, 0x11, kPriv);
  memset(priv, 0x11, kPriv);
  memset(stackbuf, 0x11, sizeof(stackbuf));
  g_data = 0x11111111;
  g_bss = 0x11111111;
  shared[0] = 0x11;

  int p2c[2], c2p[2];
  if (pipe(p2c) < 0 || pipe(c2p) < 0) { perror("pipe"); exit(1); }

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    /* The child starts out seeing exactly what the parent had. */
    CHILD_CHECK(g_data == 0x11111111, f, ".data not inherited");
    CHILD_CHECK(g_bss == 0x11111111, f, ".bss not inherited");
    CHILD_CHECK(heap[0] == 0x11 && heap[kPriv - 1] == 0x11, f,
                "heap not inherited");
    CHILD_CHECK(priv[0] == 0x11 && priv[kPriv - 1] == 0x11, f,
                "MAP_PRIVATE not inherited");
    CHILD_CHECK(stackbuf[0] == 0x11, f, "stack not inherited");
    CHILD_CHECK(shared[0] == 0x11, f, "MAP_SHARED not inherited");

    /* Now diverge. */
    g_data = 0x22222222;
    g_bss = 0x22222222;
    memset(heap, 0x22, kPriv);
    memset(priv, 0x22, kPriv);
    memset(stackbuf, 0x22, sizeof(stackbuf));
    shared[0] = 0x22;

    sync_send(c2p[1], 'a');
    sync_recv(p2c[0]);

    /* The parent has since written 0x33 everywhere. None of its private
     * writes may be visible here; its shared write must be. */
    CHILD_CHECK(g_data == 0x22222222, f, ".data leaked parent write (0x%x)",
                g_data);
    CHILD_CHECK(g_bss == 0x22222222, f, ".bss leaked parent write");
    CHILD_CHECK(heap[0] == 0x22 && heap[kPriv - 1] == 0x22, f,
                "heap leaked parent write");
    CHILD_CHECK(priv[0] == 0x22 && priv[kPriv - 1] == 0x22, f,
                "MAP_PRIVATE leaked parent write");
    CHILD_CHECK(stackbuf[0] == 0x22, f, "stack leaked parent write");
    CHILD_CHECK(shared[0] == 0x33, f,
                "MAP_SHARED did not observe parent write (0x%x)", shared[0]);
    _exit(f);
  }

  sync_recv(c2p[0]);
  /* The child has written 0x22 everywhere. The parent must be untouched. */
  CHECK(g_data == 0x11111111, ".data unaffected by child write (0x%x)", g_data);
  CHECK(g_bss == 0x11111111, ".bss unaffected by child write");
  CHECK(heap[0] == 0x11 && heap[kPriv - 1] == 0x11,
        "heap unaffected by child write");
  CHECK(priv[0] == 0x11 && priv[kPriv - 1] == 0x11,
        "MAP_PRIVATE unaffected by child write");
  CHECK(stackbuf[0] == 0x11, "stack unaffected by child write");
  CHECK(shared[0] == 0x22, "MAP_SHARED observed child write (0x%x)", shared[0]);

  g_data = 0x33333333;
  g_bss = 0x33333333;
  memset(heap, 0x33, kPriv);
  memset(priv, 0x33, kPriv);
  memset(stackbuf, 0x33, sizeof(stackbuf));
  shared[0] = 0x33;
  sync_send(p2c[1], 'b');

  int status;
  xwaitpid(pid, &status);
  absorb_child(status, "copy-on-write checks in child");

  close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
  free(heap);
  munmap(priv, kPriv);
  munmap(shared, 4096);
}

static void test_new_mappings_are_private(void) {
  section("address space: mappings created after fork are not shared");

  int c2p[2];
  if (pipe(c2p) < 0) { perror("pipe"); exit(1); }

  /* Reserve an address the child will map into, so the parent can probe it. */
  size_t len = 64 * 1024;
  void *hint = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (hint == MAP_FAILED) { perror("mmap"); exit(1); }
  munmap(hint, len);

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    void *p = mmap(hint, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHILD_CHECK(p == hint, f, "child could not map at the reserved address");
    if (p != MAP_FAILED) memset(p, 0x5a, len);
    sync_send(c2p[1], 'a');
    /* Give the parent time to probe before the mapping goes away. */
    sleep_ms(200);
    _exit(f);
  }
  sync_recv(c2p[0]);

  /*
   * The parent must not see the child's mapping. mincore() would answer this
   * directly but is not universally implemented, so touch the address and
   * require it to fault.
   */
  bool faulted = probe_unmapped(hint);
  CHECK(faulted, "child's new mapping is invisible in the parent");

  int status;
  xwaitpid(pid, &status);
  absorb_child(status, "post-fork mapping checks in child");
  close(c2p[0]); close(c2p[1]);
}

static void test_fds(void) {
  section("descriptors: inheritance, shared offset, O_CLOEXEC survives fork");

  char path[] = "/tmp/fork_stress_fd_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) { perror("mkstemp"); exit(1); }
  unlink(path);
  xwrite(fd, "0123456789abcdef", 16);
  lseek(fd, 0, SEEK_SET);

  int cloexec = dup(fd);
  if (cloexec >= 0) fcntl(cloexec, F_SETFD, FD_CLOEXEC);

  int p2c[2], c2p[2];
  if (pipe(p2c) < 0 || pipe(c2p) < 0) { perror("pipe"); exit(1); }

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    char buf[4];
    CHILD_CHECK(xread(fd, buf, 4) == 4 && memcmp(buf, "0123", 4) == 0, f,
                "child could not read the inherited descriptor");
    CHILD_CHECK(fcntl(cloexec, F_GETFD) >= 0, f,
                "FD_CLOEXEC descriptor was closed by fork (it must not be)");
    CHILD_CHECK((fcntl(cloexec, F_GETFD) & FD_CLOEXEC) != 0, f,
                "FD_CLOEXEC flag not inherited");
    sync_send(c2p[1], 'a');
    sync_recv(p2c[0]);
    /* Parent advanced the shared offset to 8. */
    off_t off = lseek(fd, 0, SEEK_CUR);
    CHILD_CHECK(off == 8, f,
                "file offset is not shared with the parent (child sees %ld)",
                (long)off);
    _exit(f);
  }

  sync_recv(c2p[0]);
  /* The child read 4 bytes; the offset is shared, so the parent sees 4. */
  off_t off = lseek(fd, 0, SEEK_CUR);
  CHECK(off == 4, "file offset shared with child (parent sees %ld, want 4)",
        (long)off);
  lseek(fd, 8, SEEK_SET);
  sync_send(p2c[1], 'b');

  int status;
  xwaitpid(pid, &status);
  absorb_child(status, "descriptor checks in child");

  close(fd);
  if (cloexec >= 0) close(cloexec);
  close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
}

static volatile sig_atomic_t sigusr1_count;
static void sigusr1_handler(int signo) { (void)signo; sigusr1_count++; }

static void test_signals(void) {
  section("signals: handlers and mask inherited, pending signals cleared");

  struct sigaction sa, old;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigusr1_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, &old);

  sigset_t block, oldmask;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR2);
  sigprocmask(SIG_BLOCK, &block, &oldmask);

  /* Make a SIGUSR2 pending in the parent; it must not be inherited. */
  raise(SIGUSR2);

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    CHILD_CHECK(sigismember(&cur, SIGUSR2) == 1, f,
                "signal mask not inherited by child");

    sigset_t pend;
    sigpending(&pend);
    CHILD_CHECK(sigismember(&pend, SIGUSR2) == 0, f,
                "pending signal was inherited by child (it must not be)");

    struct sigaction cur_sa;
    sigaction(SIGUSR1, NULL, &cur_sa);
    CHILD_CHECK(cur_sa.sa_handler == sigusr1_handler, f,
                "signal handler not inherited by child");

    CHILD_CHECK(sigusr1_count == 0, f, "unexpected inherited handler state");
    raise(SIGUSR1);
    CHILD_CHECK(sigusr1_count == 1, f,
                "inherited handler did not run in child");
    _exit(f);
  }

  int status;
  xwaitpid(pid, &status);
  absorb_child(status, "signal state checks in child");

  sigset_t pend;
  sigpending(&pend);
  CHECK(sigismember(&pend, SIGUSR2) == 1,
        "parent's own pending SIGUSR2 survived the fork");

  /*
   * Unblocking SIGUSR2 with one still pending would terminate us, so discard
   * it first: setting a signal to SIG_IGN drops any pending instance.
   */
  struct sigaction ign, old_usr2;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  sigaction(SIGUSR2, &ign, &old_usr2);
  sigprocmask(SIG_SETMASK, &oldmask, NULL);
  sigaction(SIGUSR2, &old_usr2, NULL);
  sigaction(SIGUSR1, &old, NULL);
}

static void test_exit_status(void) {
  section("exit status: normal exit codes and death by signal");

  pid_t pid = fork();
  if (pid == 0) _exit(42);
  int status;
  xwaitpid(pid, &status);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42,
        "child _exit(42) reported as exit code 42 (status 0x%x)", status);

  pid = fork();
  if (pid == 0) {
    signal(SIGTERM, SIG_DFL);
    raise(SIGTERM);
    _exit(0);
  }
  xwaitpid(pid, &status);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
        "child killed by SIGTERM reported as signalled (status 0x%x)", status);

  /* A second wait on a reaped pid must fail with ECHILD. */
  errno = 0;
  pid_t r = waitpid(pid, &status, WNOHANG);
  CHECK(r < 0 && errno == ECHILD,
        "waiting again on a reaped child fails with ECHILD (rc=%d errno=%d)",
        (int)r, errno);
}

static void test_wait_bad_pointer(void) {
  section("wait: a bad status pointer reports EFAULT and still reaps");

  pid_t pid = fork();
  if (pid == 0) _exit(3);

  /*
   * Linux reaps the child and only then copies the status out, so a bad
   * pointer costs the caller the exit status entirely -- the child is gone and
   * a second wait reports ECHILD. It is a wart, but it is the behaviour, and a
   * program that passes a bad pointer must not take the system down.
   */
  errno = 0;
  pid_t r = waitpid(pid, (int *)(uintptr_t)2, 0);
  CHECK(r < 0 && errno == EFAULT,
        "waitpid with a bad status pointer fails with EFAULT (rc=%d errno=%d)",
        (int)r, errno);

  int status = 0;
  errno = 0;
  r = waitpid(pid, &status, 0);
  CHECK(r < 0 && errno == ECHILD,
        "the child was reaped anyway, so waiting again gives ECHILD "
        "(rc=%d errno=%d)", (int)r, errno);
}

static void test_itimer_reset(void) {
  section("timers: interval timers are not inherited");

  struct itimerval it, got;
  memset(&it, 0, sizeof(it));
  it.it_value.tv_sec = 100;
  if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
    printf("  skip: setitimer unsupported (%s)\n", strerror(errno));
    return;
  }

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    struct itimerval cit;
    memset(&cit, 0, sizeof(cit));
    getitimer(ITIMER_REAL, &cit);
    CHILD_CHECK(cit.it_value.tv_sec == 0 && cit.it_value.tv_usec == 0, f,
                "ITIMER_REAL was inherited by child (%ld.%06ld left)",
                (long)cit.it_value.tv_sec, (long)cit.it_value.tv_usec);
    _exit(f);
  }
  int status;
  xwaitpid(pid, &status);
  absorb_child(status, "itimer checks in child");

  getitimer(ITIMER_REAL, &got);
  CHECK(got.it_value.tv_sec > 0, "parent's own timer still armed");

  memset(&it, 0, sizeof(it));
  setitimer(ITIMER_REAL, &it, NULL);
}

static void test_rusage_reset(void) {
  section("accounting: child CPU time starts near zero");

  burn_cpu_ms(150);

  struct rusage pru;
  memset(&pru, 0, sizeof(pru));
  if (getrusage(RUSAGE_SELF, &pru) < 0) {
    printf("  skip: getrusage unsupported (%s)\n", strerror(errno));
    return;
  }
  uint64_t parent_us = (uint64_t)pru.ru_utime.tv_sec * 1000000 +
                       pru.ru_utime.tv_usec +
                       (uint64_t)pru.ru_stime.tv_sec * 1000000 +
                       pru.ru_stime.tv_usec;

  int fds[2];
  if (pipe(fds) < 0) { perror("pipe"); exit(1); }

  pid_t pid = fork();
  if (pid == 0) {
    struct rusage cru;
    memset(&cru, 0, sizeof(cru));
    getrusage(RUSAGE_SELF, &cru);
    uint64_t child_us = (uint64_t)cru.ru_utime.tv_sec * 1000000 +
                        cru.ru_utime.tv_usec +
                        (uint64_t)cru.ru_stime.tv_sec * 1000000 +
                        cru.ru_stime.tv_usec;
    xwrite(fds[1], &child_us, sizeof(child_us));
    _exit(0);
  }
  uint64_t child_us = UINT64_MAX;
  xread(fds[0], &child_us, sizeof(child_us));
  int status;
  xwaitpid(pid, &status);

  CHECK(parent_us > 50000,
        "parent accumulated measurable CPU time (%llu us)",
        (unsigned long long)parent_us);
  CHECK(child_us * 2 < parent_us,
        "child's CPU accounting was reset (child %llu us vs parent %llu us)",
        (unsigned long long)child_us, (unsigned long long)parent_us);
  close(fds[0]); close(fds[1]);
}

/* --- thread collapse ---------------------------------------------------- */

struct ticker_arg {
  atomic_ullong counter;
  atomic_int stop;
};

static void *ticker_thread(void *p) {
  struct ticker_arg *t = p;
  while (!atomic_load(&t->stop)) {
    atomic_fetch_add(&t->counter, 1);
    struct timespec ts = {0, 200 * 1000}; /* 200 us */
    nanosleep(&ts, NULL);
  }
  return NULL;
}

static void test_thread_collapse(void) {
  section("threads: only the calling thread survives into the child");

  /*
   * The ticker lives in a MAP_SHARED page so the child can observe whether a
   * copy of the ticker thread is still running in its own address space.
   */
  struct ticker_arg *t = mmap(NULL, sizeof(*t), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (t == MAP_FAILED) { perror("mmap"); exit(1); }
  atomic_init(&t->counter, 0);
  atomic_init(&t->stop, 0);

  pthread_t th;
  if (pthread_create(&th, NULL, ticker_thread, t) != 0) {
    perror("pthread_create");
    exit(1);
  }

  /* Let it get going. */
  sleep_ms(50);
  CHECK(atomic_load(&t->counter) > 0, "ticker thread is running in the parent");

  int c2p[2];
  if (pipe(c2p) < 0) { perror("pipe"); exit(1); }

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    /*
     * Stop the parent's ticker via the shared flag, wait, then check the
     * counter has gone quiet. If a copy of the thread had survived into the
     * child it would keep incrementing after the flag was cleared again.
     */
    atomic_store(&t->stop, 1);
    sleep_ms(100);
    unsigned long long a = atomic_load(&t->counter);
    sleep_ms(100);
    unsigned long long b = atomic_load(&t->counter);
    CHILD_CHECK(a == b, f,
                "a second ticker thread is running (counter moved %llu -> %llu "
                "after stop)", a, b);
    /* Best effort: procfs agrees the child has one thread. */
    FILE *fp = fopen("/proc/self/status", "r");
    if (fp) {
      char line[256];
      while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Threads:", 8) == 0) {
          int n = atoi(line + 8);
          CHILD_CHECK(n == 1, f, "/proc/self/status reports %d threads, want 1",
                      n);
          break;
        }
      }
      fclose(fp);
    }
    sync_send(c2p[1], 'a');
    _exit(f);
  }

  sync_recv(c2p[0]);
  int status;
  xwaitpid(pid, &status);
  absorb_child(status, "thread collapse checks in child");

  atomic_store(&t->stop, 1);
  pthread_join(th, NULL);
  munmap(t, sizeof(*t));
  close(c2p[0]); close(c2p[1]);
}

/* --- fork from a non-main thread ---------------------------------------- */

struct forker_arg { int failures; pid_t parent_pid; };

static void *forker_thread(void *p) {
  struct forker_arg *a = p;
  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    CHILD_CHECK(getppid() == a->parent_pid, f,
                "child of a non-main thread has ppid %d, want %d", getppid(),
                a->parent_pid);
    _exit(f);
  }
  if (pid < 0) { a->failures++; return NULL; }
  int status;
  xwaitpid(pid, &status);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) a->failures++;
  return NULL;
}

static void test_fork_from_thread(void) {
  section("threads: fork from a non-main thread");

  struct forker_arg a = {0, getpid()};
  pthread_t th;
  if (pthread_create(&th, NULL, forker_thread, &a) != 0) {
    perror("pthread_create");
    exit(1);
  }
  pthread_join(th, NULL);
  CHECK(a.failures == 0, "fork from a non-main thread worked");
}

/* --- pthread_atfork ------------------------------------------------------ */

static int atfork_prepare_count, atfork_parent_count;
static volatile int atfork_child_count;

static void af_prepare(void) { atfork_prepare_count++; }
static void af_parent(void) { atfork_parent_count++; }
static void af_child(void) { atfork_child_count++; }

static void test_atfork(void) {
  section("pthread_atfork: prepare/parent/child handlers");

  if (pthread_atfork(af_prepare, af_parent, af_child) != 0) {
    printf("  skip: pthread_atfork unsupported\n");
    return;
  }
  int before_prepare = atfork_prepare_count;
  int before_parent = atfork_parent_count;

  pid_t pid = fork();
  if (pid == 0) {
    int f = 0;
    CHILD_CHECK(atfork_child_count == 1, f,
                "child atfork handler ran %d times, want 1", atfork_child_count);
    CHILD_CHECK(atfork_parent_count == before_parent, f,
                "parent atfork handler ran in the child");
    _exit(f);
  }
  int status;
  xwaitpid(pid, &status);
  absorb_child(status, "atfork handler checks in child");

  CHECK(atfork_prepare_count == before_prepare + 1,
        "prepare handler ran once in the parent");
  CHECK(atfork_parent_count == before_parent + 1,
        "parent handler ran once in the parent");
  CHECK(atfork_child_count == 0, "child handler did not run in the parent");
}

/* --- orphan reparenting -------------------------------------------------- */

struct orphan_slot {
  volatile pid_t middle_pid;
  volatile pid_t orig_ppid;
  volatile pid_t new_ppid;
  volatile int sampled; /* grandchild has recorded orig_ppid */
  volatile int done;
};

static void test_orphan_reparent(void) {
  section("lifecycle: orphans are reparented");

  struct orphan_slot *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (s == MAP_FAILED) { perror("mmap"); exit(1); }
  s->done = 0;
  s->sampled = 0;

  pid_t pid = fork();
  if (pid == 0) {
    /*
     * Middle process: fork a grandchild, wait until it has recorded its
     * original parent, then exit. Without that handshake the middle process
     * can exit before the grandchild ever runs, and the reparenting has
     * already happened by the time it looks.
     */
    s->middle_pid = getpid();
    pid_t g = fork();
    if (g == 0) {
      pid_t orig = getppid();
      s->orig_ppid = orig;
      s->sampled = 1;
      uint64_t deadline = monotonic_ns() + 5ULL * 1000000000ULL;
      pid_t now = orig;
      while (monotonic_ns() < deadline) {
        now = getppid();
        if (now != orig) break;
        sleep_ms(2);
      }
      s->new_ppid = now;
      s->done = 1;
      _exit(0);
    }
    uint64_t deadline = monotonic_ns() + 5ULL * 1000000000ULL;
    while (!s->sampled && monotonic_ns() < deadline) {
      struct timespec ts = {0, 1000 * 1000};
      nanosleep(&ts, NULL);
    }
    _exit(0);
  }
  int status;
  xwaitpid(pid, &status);

  uint64_t deadline = monotonic_ns() + 8ULL * 1000000000ULL;
  while (!s->done && monotonic_ns() < deadline) {
    sleep_ms(2);
  }

  CHECK(s->done, "orphaned grandchild reported in");
  if (s->done) {
    CHECK(s->orig_ppid == s->middle_pid && s->middle_pid == pid,
          "grandchild's original ppid was the middle process (%d, want %d)",
          s->orig_ppid, pid);
    CHECK(s->new_ppid != s->orig_ppid,
          "grandchild was reparented after its parent exited (ppid now %d)",
          s->new_ppid);
  }
  munmap(s, sizeof(*s));
}

/* --- nesting ------------------------------------------------------------- */

static void test_nested_fork(int depth) {
  section("nesting: a chain of forks");

  int fds[2];
  if (pipe(fds) < 0) { perror("pipe"); exit(1); }

  pid_t pid = fork();
  if (pid == 0) {
    int level = 1;
    while (level < depth) {
      pid_t c = fork();
      if (c < 0) break;
      if (c > 0) {
        int st;
        xwaitpid(c, &st);
        /* Propagate the deepest level reached back up the chain. */
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 0);
      }
      level++;
    }
    _exit(level > 255 ? 255 : level);
  }
  int status;
  xwaitpid(pid, &status);
  int reached = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  CHECK(reached == depth, "fork chain reached depth %d, want %d", reached,
        depth);
  close(fds[0]); close(fds[1]);
}

/* ---------------------------------------------------------------------- */
/* Part 2: stress                                                         */
/* ---------------------------------------------------------------------- */

/*
 * Each stress child verifies a checksum over its inherited private memory and
 * then scribbles on it. If the clone mechanism ever aliased two address spaces
 * -- the failure mode that matters most for a new implementation -- a
 * concurrent sibling's scribble would corrupt the checksum and be caught here.
 */
struct stress_ctx {
  char *region;
  size_t region_len;
  uint64_t expect_sum;
  atomic_ullong forks;
  atomic_ullong failures;
  atomic_int stop;
  unsigned per_child_touch_pages;
};

static void stress_child(struct stress_ctx *ctx, unsigned seed) {
  int f = 0;
  uint64_t sum = checksum(ctx->region, ctx->region_len);
  CHILD_CHECK(sum == ctx->expect_sum, f,
              "inherited memory checksum mismatch (got %llx want %llx)",
              (unsigned long long)sum, (unsigned long long)ctx->expect_sum);

  /* Dirty some pages so the child exercises copy-on-write faults. */
  size_t pages = ctx->region_len / 4096;
  for (unsigned i = 0; i < ctx->per_child_touch_pages && pages; i++) {
    seed = seed * 1103515245u + 12345u;
    ctx->region[(seed % pages) * 4096] = (char)(seed >> 16);
  }
  _exit(f);
}

static void *stress_worker(void *p) {
  struct stress_ctx *ctx = p;
  unsigned seed = (unsigned)(uintptr_t)pthread_self();
  while (!atomic_load(&ctx->stop)) {
    pid_t pid = fork();
    if (pid < 0) {
      if (errno == EAGAIN || errno == ENOMEM) {
        sched_yield();
        continue;
      }
      atomic_fetch_add(&ctx->failures, 1);
      break;
    }
    if (pid == 0) stress_child(ctx, seed++);
    int status;
    if (xwaitpid(pid, &status) != pid) {
      atomic_fetch_add(&ctx->failures, 1);
      continue;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      atomic_fetch_add(&ctx->failures, 1);
    atomic_fetch_add(&ctx->forks, 1);
  }
  return NULL;
}

static void stress_storm(unsigned nthreads, unsigned seconds, size_t region_kb,
                         unsigned touch_pages) {
  printf("[ stress: %u thread(s), %u s, %zu KB private region, %u COW pages/child ]\n",
         nthreads, seconds, region_kb, touch_pages);
  fflush(stdout);

  struct stress_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.region_len = region_kb * 1024;
  ctx.per_child_touch_pages = touch_pages;
  ctx.region = mmap(NULL, ctx.region_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ctx.region == MAP_FAILED) { perror("mmap"); exit(1); }
  for (size_t i = 0; i < ctx.region_len; i++)
    ctx.region[i] = (char)(i * 31 + 7);
  ctx.expect_sum = checksum(ctx.region, ctx.region_len);
  atomic_init(&ctx.forks, 0);
  atomic_init(&ctx.failures, 0);
  atomic_init(&ctx.stop, 0);

  pthread_t *th = calloc(nthreads, sizeof(*th));
  uint64_t t0 = monotonic_ns();
  for (unsigned i = 0; i < nthreads; i++) {
    if (pthread_create(&th[i], NULL, stress_worker, &ctx) != 0) {
      perror("pthread_create");
      exit(1);
    }
  }

  sleep_ms(seconds * 1000);
  atomic_store(&ctx.stop, 1);
  for (unsigned i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
  uint64_t elapsed = monotonic_ns() - t0;

  unsigned long long forks = atomic_load(&ctx.forks);
  unsigned long long fails = atomic_load(&ctx.failures);
  double secs = (double)elapsed / 1e9;
  printf("  %llu forks in %.2f s = %.0f forks/s (%.1f us/fork/thread)\n", forks,
         secs, (double)forks / secs,
         secs * 1e6 * nthreads / (double)(forks ? forks : 1));

  /* The parent's own copy must be pristine after thousands of children. */
  CHECK(checksum(ctx.region, ctx.region_len) == ctx.expect_sum,
        "parent's private region survived %llu concurrent forks intact", forks);
  CHECK(fails == 0, "no child reported a failure (%llu failures)", fails);
  CHECK(forks > 0, "made forward progress");

  free(th);
  munmap(ctx.region, ctx.region_len);
}

static void stress_fanout(unsigned width) {
  printf("[ stress: simultaneous fan-out of %u children ]\n", width);
  fflush(stdout);

  /* Each child gets a distinct token, writes it over its private copy, and
   * returns a digest of what it sees. Cross-talk shows up as a bad digest. */
  size_t len = 256 * 1024;
  char *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) { perror("mmap"); exit(1); }
  memset(region, 0xaa, len);

  pid_t *kids = calloc(width, sizeof(*kids));
  unsigned started = 0;
  for (unsigned i = 0; i < width; i++) {
    pid_t pid = fork();
    if (pid < 0) break;
    if (pid == 0) {
      unsigned char tok = (unsigned char)(i % 251 + 1);
      memset(region, tok, len);
      /* Hold every sibling live at once, then re-verify. */
      sleep_ms(30);
      for (size_t j = 0; j < len; j += 4093)
        if ((unsigned char)region[j] != tok) _exit(1);
      _exit(0);
    }
    kids[started++] = pid;
  }

  unsigned bad = 0;
  for (unsigned i = 0; i < started; i++) {
    int status;
    xwaitpid(kids[i], &status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) bad++;
  }
  CHECK(started == width, "started all %u children (%u started)", width,
        started);
  CHECK(bad == 0, "no fan-out child observed a sibling's writes (%u bad)", bad);

  for (size_t j = 0; j < len; j += 4093)
    if ((unsigned char)region[j] != 0xaa) { bad++; break; }
  CHECK(bad == 0, "parent's region unchanged after fan-out");

  free(kids);
  munmap(region, len);
}

/* ---------------------------------------------------------------------- */

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s [options]\n"
          "  -t <n>    stress threads (default 4)\n"
          "  -s <sec>  stress duration in seconds (default 5)\n"
          "  -k <KB>   private region per stress child (default 1024)\n"
          "  -c <n>    COW pages dirtied per stress child (default 16)\n"
          "  -f <n>    fan-out width (default 64)\n"
          "  -d <n>    fork chain depth (default 16)\n"
          "  -S        skip the stress phase (semantics only)\n"
          "  -O        skip the semantics phase (stress only)\n"
          "  -v        verbose: print passing checks too\n",
          prog);
  exit(2);
}

int main(int argc, char *argv[]) {
  unsigned threads = 4, seconds = 5, fanout = 64, depth = 16, touch = 16;
  size_t region_kb = 1024;
  bool skip_stress = false, skip_semantics = false;

  int opt;
  while ((opt = getopt(argc, argv, "t:s:k:c:f:d:SOvh")) != -1) {
    switch (opt) {
      case 't': threads = (unsigned)atoi(optarg); break;
      case 's': seconds = (unsigned)atoi(optarg); break;
      case 'k': region_kb = strtoul(optarg, NULL, 0); break;
      case 'c': touch = (unsigned)atoi(optarg); break;
      case 'f': fanout = (unsigned)atoi(optarg); break;
      case 'd': depth = (unsigned)atoi(optarg); break;
      case 'S': skip_stress = true; break;
      case 'O': skip_semantics = true; break;
      case 'v': verbose = true; break;
      default: usage(argv[0]);
    }
  }

  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("fork semantics and stress test (pid %d)\n\n", getpid());

  if (!skip_semantics) {
  test_identity();
  test_cow();
  test_new_mappings_are_private();
  test_fds();
  test_signals();
  test_exit_status();
  test_wait_bad_pointer();
  test_itimer_reset();
  test_rusage_reset();
  test_thread_collapse();
  test_fork_from_thread();
  test_atfork();
  test_orphan_reparent();
  test_nested_fork((int)depth);
  }

  if (!skip_stress) {
    stress_fanout(fanout);
    stress_storm(threads, seconds, region_kb, touch);
    stress_storm(threads, seconds, 16, 4); /* tiny footprint, max rate */
  }

  printf("\n%d checks, %d failure(s)\n", checks_run, checks_failed);
  printf("%s\n", checks_failed == 0 ? "PASS" : "FAIL");
  return checks_failed == 0 ? 0 : 1;
}
