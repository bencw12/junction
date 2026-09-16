# Bug: `log_*_ratelimited` swallows everything in the first second

**Status:** fixed. `lib/caladan/inc/base/log.h`.

## What was wrong

```c
#define log_ratelimited(level, fmt, ...)		\
({							\
	static uint64_t __last_us = 0;			\
	static uint64_t __suppressed = 0;		\
	uint64_t __cur_us = microtime();		\
	if (__cur_us - __last_us >= ONE_SECOND) {	\
		...					\
	} else						\
		__suppressed++;				\
})
```

`__last_us` starts at 0 and `microtime()` counts from runtime start, so for the
first second of a process's life `__cur_us - __last_us` is below `ONE_SECOND`
and the branch is never taken. **The first occurrence was suppressed along with
the flood.** A rate limiter is supposed to do the opposite: log the first, drop
the rest.

Nothing logged this way during startup ever appeared. Caladan's whole
initialisation happens inside that window — the traces here show it completing
by ~41 ms.

## Why it mattered here

Both address-space hazards found in this audit are announced by exactly this
mechanism, and both messages had never been seen:

```c
/* runtime/stack.c */
log_warn_ratelimited("stack: high-water mark exceeded the reserved pool of %d "
                     "stacks; new stacks will not be visible in address "
                     "spaces cloned earlier\n", ...);

/* base/page.c */
log_warn_ratelimited("page: growing the heap past the reserved pool; new "
                     "memory will not be visible in address spaces cloned "
                     "earlier\n");
```

The stack one stayed silent while the path was taken **350 times in a single
run**; the page one while it was taken 4 times. Finding the overflow at all
required adding a temporary unconditional `log_err`. The code knew about the
hazard, said so, and the message went nowhere.

23 call sites are affected across `runtime/`, `base/` and `iokernel/`, including
`net: out of mbufs`, `net: out of tx buffers`, and
`stack: failed to allocate stack memory`.

## The fix

Track whether anything has been logged yet, rather than inferring it from a
timestamp that is indistinguishable from "logged at time zero":

```c
static bool __ever_logged = false;
...
if (!__ever_logged || __cur_us - __last_us >= ONE_SECOND) {
	...
	__ever_logged = true;
	__last_us = __cur_us;
}
```

One byte of static state per call site, and the first occurrence is always
reported.

Rejected alternative: initialising `__last_us` to `-ONE_SECOND` and relying on
unsigned wraparound. It works, but "the first log happens because an unsigned
subtraction overflows" is not a thing to leave for the next reader.

## Verified

Both previously-silent messages now appear on the first occurrence:

```
page: growing the heap past the reserved pool; new memory is visible only in
      the address space that allocated it
stack: high-water mark exceeded the reserved pool of 1 stacks; new stacks are
       visible only in the address space that allocated them
```

Both were also raised from `log_warn` to `log_err`, since what they report is a
correctness hazard rather than a warning.
