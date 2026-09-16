/*
 * as_test.c - standalone exercise for the junction_as kernel module.
 *
 * Proves the property Junction needs: one thread of one process can run in
 * several address spaces, each a copy-on-write snapshot of the original, with
 * private memory isolated between them and MAP_SHARED memory coherent across
 * them.
 *
 * There is one non-obvious rule that this test also demonstrates. A thread
 * that switches address spaces keeps using its own stack across the switch, so
 * the stack must be mapped identically in every address space -- MAP_SHARED,
 * not MAP_PRIVATE. If it were private, the frames pushed while running in one
 * address space would simply not be there after switching back, and the return
 * from the switch would jump into whatever the other copy of the stack held.
 * The worker thread below therefore runs on an explicitly shared stack (glibc
 * places its TLS block at the top of a caller-provided stack, so that becomes
 * shared too).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "junction_as.h"

#ifndef __WCLONE
#define __WCLONE 0x80000000
#endif

#define REGION_LEN (1UL << 20) /* 1 MB */
#define NR_SPACES 4

/*
 * A slot of the address space whose page tables are shared with every address
 * space created afterwards. Picked well clear of anything the loader uses, and
 * aligned to a paging slot (512 GB) because that is the granularity at which
 * tables can be shared.
 */
#define SHARED_BASE 0x600000000000UL
#define SHARED_MAGIC 0xfeedfacecafebeefUL

static int dev_fd = -1;
static int failures;
static int checks;

#define CHECK(cond, ...)                          \
	do {                                      \
		checks++;                         \
		if (!(cond)) {                    \
			failures++;               \
			printf("  FAIL: ");       \
			printf(__VA_ARGS__);      \
			printf("  (%s:%d)\n", __FILE__, __LINE__); \
		} else {                          \
			printf("  ok: ");         \
			printf(__VA_ARGS__);      \
			printf("\n");             \
		}                                 \
		fflush(stdout);                   \
	} while (0)

/* Shared state the worker and main thread both touch. */
struct shared {
	volatile unsigned long seq;
	volatile unsigned long worker_wrote;
};

static struct shared *shr;
static unsigned char *region;	/* MAP_PRIVATE, copy-on-write across spaces */

static unsigned char pattern_of(size_t i, unsigned char tag)
{
	return (unsigned char)((i * 31u + 7u) ^ tag);
}

static void fill_region(unsigned char tag)
{
	for (size_t i = 0; i < REGION_LEN; i += 4096)
		region[i] = pattern_of(i, tag);
}

static int region_matches(unsigned char tag)
{
	for (size_t i = 0; i < REGION_LEN; i += 4096)
		if (region[i] != pattern_of(i, tag))
			return 0;
	return 1;
}

/* ---------------------------------------------------------------------- */

/*
 * Reads a word, reporting whether the access faulted rather than dying. If
 * page table sharing is not working, the address is simply absent from the
 * other address space, and we want a failed check instead of a SIGSEGV.
 */
static sigjmp_buf probe_jmp;
static volatile sig_atomic_t probe_faulted;

static void probe_handler(int signo) {
	(void)signo;
	probe_faulted = 1;
	siglongjmp(probe_jmp, 1);
}

static int read_guarded(volatile unsigned long *addr, unsigned long *out) {
	struct sigaction sa, old;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = probe_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NODEFER;
	sigaction(SIGSEGV, &sa, &old);

	probe_faulted = 0;
	if (sigsetjmp(probe_jmp, 1) == 0)
		*out = *addr;

	sigaction(SIGSEGV, &old, NULL);
	return probe_faulted ? -1 : 0;
}

static long as_share_range(unsigned long start, unsigned long len) {
	struct junction_as_share a;
	memset(&a, 0, sizeof(a));
	a.start = start;
	a.len = len;
	return syscall(SYS_ioctl, dev_fd, JUNCTION_AS_SHARE_RANGE, &a);
}

static long as_switch(unsigned long handle)
{
	return syscall(SYS_ioctl, dev_fd, JUNCTION_AS_SWITCH, handle);
}

/*
 * Creates a new address space: clone the process so Linux performs the
 * copy-on-write duplication, adopt the clone's mm, then discard the clone.
 *
 * The clone shares our descriptor table and filesystem context, so it costs
 * nothing but a task_struct and the duplicated page tables.
 */
static unsigned long as_create(void)
{
	struct junction_as_adopt arg;
	long pid;
	int rc;

	pid = syscall(SYS_clone, (unsigned long)(CLONE_FILES | CLONE_FS |
						 CLONE_SYSVSEM),
		      0UL, 0UL, 0UL, 0UL);
	if (pid < 0) {
		perror("clone");
		return 0;
	}
	if (pid == 0) {
		/*
		 * Stay alive and off the CPU until the parent has taken a
		 * reference to this address space. Never returns.
		 */
		for (;;)
			syscall(SYS_pause);
	}

	memset(&arg, 0, sizeof(arg));
	arg.pid = (unsigned int)pid;
	rc = (int)syscall(SYS_ioctl, dev_fd, JUNCTION_AS_ADOPT, &arg);
	if (rc < 0) {
		perror("ioctl(ADOPT)");
		arg.handle = 0;
	}

	syscall(SYS_kill, pid, SIGKILL);
	while (syscall(SYS_wait4, pid, NULL, __WCLONE, NULL) < 0 &&
	       errno == EINTR)
		;

	return (unsigned long)arg.handle;
}

/* ---------------------------------------------------------------------- */

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *worker(void *arg)
{
	unsigned long spaces[NR_SPACES];
	unsigned long cur = 0;
	int sharing = 1;
	long rc;

	(void)arg;

	/*
	 * Page table sharing is an experiment behind a module parameter
	 * (enable_pgtable_sharing=1). When it is off the module reports
	 * EOPNOTSUPP and the checks that depend on it are skipped; everything
	 * else still runs, because the switch mechanism does not depend on it.
	 */
	printf("[ shared page tables: registration ]\n");
	rc = as_share_range(SHARED_BASE, 1UL << 20);
	if (rc < 0 && errno == EOPNOTSUPP) {
		printf("  skip: page table sharing is disabled in the module\n");
		sharing = 0;
	} else {
		CHECK(rc == 0, "registered a shared range (rc=%ld errno=%d)", rc,
		      errno);
		if (rc != 0)
			return NULL;
	}

	printf("[ address space creation ]\n");
	for (int i = 0; i < NR_SPACES; i++) {
		spaces[i] = as_create();
		CHECK(spaces[i] != 0, "created address space %d (handle %lu)", i,
		      spaces[i]);
		if (!spaces[i])
			return NULL;
	}

	printf("[ isolation ]\n");
	/*
	 * Every space starts as a snapshot of the parent, which currently
	 * holds tag 0. Give each space its own tag and confirm none of them
	 * observes another's writes.
	 */
	for (int i = 0; i < NR_SPACES; i++) {
		rc = as_switch(spaces[i]);
		if (rc < 0) {
			CHECK(0, "switch to space %d failed: %s", i,
			      strerror(errno));
			return NULL;
		}
		CHECK(region_matches(0),
		      "space %d starts as a copy of the parent", i);
		fill_region((unsigned char)(i + 1));
		CHECK(region_matches((unsigned char)(i + 1)),
		      "space %d sees its own writes", i);
	}

	rc = as_switch(JUNCTION_AS_ROOT);
	CHECK(rc == 0, "switched back to the original address space");
	CHECK(region_matches(0),
	      "original address space is untouched by all %d children",
	      NR_SPACES);

	for (int i = 0; i < NR_SPACES; i++) {
		as_switch(spaces[i]);
		CHECK(region_matches((unsigned char)(i + 1)),
		      "space %d still holds only its own writes", i);
	}
	as_switch(JUNCTION_AS_ROOT);

	printf("[ shared memory coherence ]\n");
	shr->worker_wrote = 0;
	for (int i = 0; i < NR_SPACES; i++) {
		as_switch(spaces[i]);
		shr->worker_wrote += (unsigned long)(i + 1);
	}
	as_switch(JUNCTION_AS_ROOT);
	CHECK(shr->worker_wrote == (NR_SPACES * (NR_SPACES + 1)) / 2,
	      "MAP_SHARED writes from every space are visible in the original "
	      "(%lu)", shr->worker_wrote);

	printf("[ shared page tables: a mapping made after the spaces existed ]\n");
	if (!sharing) {
		printf("  skip: page table sharing is disabled\n");
	} else {
		volatile unsigned long *late;
		unsigned long v;
		int seen = 0, faulted = 0;

		/*
		 * This is the case pre-allocating pools cannot cover: memory
		 * mapped once the address spaces already exist. Without shared
		 * page tables it lives only here and faults everywhere else.
		 */
		late = mmap((void *)(SHARED_BASE + 0x1000), 4096,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		CHECK(late != MAP_FAILED, "mapped into the shared slot after the "
		      "address spaces were created");
		if (late == MAP_FAILED)
			return NULL;

		/* Keep later clones out of the shared tables. */
		madvise((void *)late, 4096, MADV_DONTFORK);
		*late = SHARED_MAGIC;

		for (int i = 0; i < NR_SPACES; i++) {
			as_switch(spaces[i]);
			v = 0;
			if (read_guarded(late, &v) < 0)
				faulted++;
			else if (v == SHARED_MAGIC)
				seen++;
		}
		as_switch(JUNCTION_AS_ROOT);
		CHECK(faulted == 0, "no address space faulted on it (%d did)",
		      faulted);
		CHECK(seen == NR_SPACES,
		      "every address space sees it (%d of %d)", seen, NR_SPACES);

		/* And the other direction: written there, seen here. */
		as_switch(spaces[1]);
		*late = SHARED_MAGIC ^ 0xffUL;
		as_switch(JUNCTION_AS_ROOT);
		CHECK(*late == (SHARED_MAGIC ^ 0xffUL),
		      "a write through another address space is visible here");

		/* Sharing must not have leaked into the private region. */
		CHECK(region_matches(0),
		      "the private region is still isolated");
	}

	printf("[ switch latency ]\n");
	{
		const int kIters = 20000;
		uint64_t t0 = now_ns();
		for (int i = 0; i < kIters; i++) {
			as_switch(spaces[i & (NR_SPACES - 1)]);
		}
		uint64_t t1 = now_ns();
		as_switch(JUNCTION_AS_ROOT);
		printf("  %.0f ns per address space switch (ioctl included)\n",
		       (double)(t1 - t0) / kIters);

		t0 = now_ns();
		for (int i = 0; i < kIters; i++)
			syscall(SYS_ioctl, dev_fd, JUNCTION_AS_CURRENT,
				&cur);
		t1 = now_ns();
		printf("  %.0f ns per no-op ioctl for comparison\n",
		       (double)(t1 - t0) / kIters);
	}

	printf("[ current handle reporting ]\n");
	as_switch(spaces[2]);
	rc = syscall(SYS_ioctl, dev_fd, JUNCTION_AS_CURRENT, &cur);
	CHECK(rc == 0 && cur == spaces[2],
	      "CURRENT reports the bound handle (%lu, want %lu)", cur,
	      spaces[2]);
	as_switch(JUNCTION_AS_ROOT);
	rc = syscall(SYS_ioctl, dev_fd, JUNCTION_AS_CURRENT, &cur);
	CHECK(rc == 0 && cur == JUNCTION_AS_ROOT,
	      "CURRENT reports the root handle after switching back (%lu)", cur);

	printf("[ teardown ]\n");
	for (int i = 0; i < NR_SPACES; i++) {
		rc = syscall(SYS_ioctl, dev_fd, JUNCTION_AS_RELEASE, spaces[i]);
		CHECK(rc == 0, "released space %d", i);
	}
	CHECK(region_matches(0), "original address space survived teardown");

	return NULL;
}

int main(void)
{
	pthread_attr_t attr;
	pthread_t th;
	void *stack;
	const size_t kStackLen = 1UL << 20;

	setvbuf(stdout, NULL, _IOLBF, 0);

	dev_fd = open(JUNCTION_AS_DEVICE, O_RDWR);
	if (dev_fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n(is junction_as.ko "
			"loaded?)\n", JUNCTION_AS_DEVICE, strerror(errno));
		return 1;
	}

	shr = mmap(NULL, sizeof(*shr), PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	region = mmap(NULL, REGION_LEN, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (shr == MAP_FAILED || region == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	fill_region(0);

	/* The switching thread must run on memory that every address space
	 * maps identically. See the comment at the top of this file. */
	stack = mmap(NULL, kStackLen, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) {
		perror("mmap(stack)");
		return 1;
	}

	pthread_attr_init(&attr);
	if (pthread_attr_setstack(&attr, stack, kStackLen) != 0) {
		perror("pthread_attr_setstack");
		return 1;
	}
	if (pthread_create(&th, &attr, worker, NULL) != 0) {
		perror("pthread_create");
		return 1;
	}
	pthread_join(th, NULL);

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	printf("%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
