# Bug: Caladan stack pool overflow crashes a forked child

**Status:** open. Reproducer and regression test landed; the fix is Phases 1-3
of `docs/fork-implementation-plan.md`.

**Reproduce:** `scripts/stack_overflow_test.sh` (fails today, 5/5).

## What happens

```
fault with preemption disabled: addr=5200055f9000 rip=... rsp=520000cf9dd0
                                pid=2 as=4294967297 want_as=4294967297
Aborting on signal: signal delivered while preemption is disabled
```

`addr` is inside `STACK_BASE_ADDR` (`0x520000000000`), the Caladan runtime stack
region. `pid=2` is a forked child, in its own address space. It is running on a
uthread stack that its address space never had mapped.

Note `want_as == as`: the address-space switch was *correct*. This is not a
scheduling bug. The mapping simply does not exist in the address space that
legitimately owns the thread.

## Why

Caladan pre-reserves `STACK_POOL_ENTRIES` (16384) stacks as one `MAP_SHARED`
mapping, so every address space sees them. That reservation is the only thing
keeping stack allocation from mapping memory into a single address space. Past
it, `stack_create()` falls through to:

```c
stack_addr = syscall_mmap(base, sizeof(struct stack), PROT_READ | PROT_WRITE,
                          (cfg_shared_runtime_mem ? MAP_SHARED : MAP_PRIVATE) |
                          MAP_ANONYMOUS, -1, 0);
```

`MAP_SHARED` makes the *contents* coherent, but the mapping is still created in
one mm. Every other address space lacks the VMA entirely.

Stacks are then recycled through a global free list, so the stack outlives the
address space that allocated it:

1. `fork()` while the pool is intact — the child's address space is cloned
   before any overflow stack exists;
2. the parent pushes the high-water mark up; those stacks are mapped into the
   parent's address space only;
3. the parent's threads exit, returning the stacks to `free_stacks`;
4. the child is handed one and runs on it.

The fault is on the thread's own stack, so there is no frame to report it from —
hence `signal delivered while preemption is disabled` rather than a normal
segfault report.

## Why it was not noticed

The pool is 16384 stacks. Reaching it takes that many *concurrent* uthreads,
which none of the existing tests do — the widest measured workload
(`fork_stress -t 16 -s 3 -f 512 -k 4096`, 512 concurrent guests) never
overflowed, and the memory trace recorded zero Caladan events across five
workloads. The pre-allocation is a bound, not a fix, and nothing was checking
the bound held.

Worse, the code already knew: `stack_create()` carried a
`log_warn_ratelimited()` saying new stacks "will not be visible in address
spaces cloned earlier". **It never appeared in output even when the path was
taken 350 times in a single run.**

That turned out to be a separate, general bug: `log_*_ratelimited` suppressed
the *first* occurrence for the first second of a process's life, which is
exactly when init runs. Fixed in `docs/bug-ratelimited-logging.md`; 23 call
sites were affected. The message here was also raised to `log_err_ratelimited`,
since what it reports is a correctness hazard rather than a warning. It now
prints:

```
stack: high-water mark exceeded the reserved pool of 1 stacks; new stacks are
       visible only in the address space that allocated them
```

## Changes made

| change | justification |
| --- | --- |
| `lib/caladan/runtime/stack.c`: `stack_pool_entries()` reads `JUNCTION_DEBUG_STACK_POOL_ENTRIES` | The pool is what hides the bug, so the overflow path is unreachable in a test without shrinking it. Env var rather than a config option to keep it clearly a debug affordance; it reaches the *identical* code path, only sooner. |
| `stack.c`: `log_warn_ratelimited` -> `log_err_ratelimited` | What it reports is a correctness hazard, so it belongs at error level. Necessary but not sufficient — the rate limiter was also broken; see below. |
| `inc/base/log.h`: `log_ratelimited` always logs the first occurrence | The reason nobody knew about either pool overflow. Separate write-up in `docs/bug-ratelimited-logging.md`. |
| `base/page.c`: same knob and same log-level change for `lgpage_create` | The large-page pool has the identical structure and backs far more (thread structs, mbufs, smalloc). Measured *not* reachable post-fork on compute workloads — see `docs/shared-memory-audit.md` — but instrumented so it is visible if it ever is. |
| `docs/traces/stack_overflow_race.c` | Encodes the fork-then-overflow-then-recycle ordering. Passes natively, so it tests Junction rather than itself. |
| `scripts/stack_overflow_test.sh` | Runs native first as a control, then Junction N times. |

## On the retries

A single attempt crashes about 80% of the time: whether the child draws a
parent-only stack depends on per-kthread tcache magazine state. The test runs
five attempts and fails on any crash, which is sound because the property is
"Junction must never crash here" — more attempts is strictly more sensitive, and
after the fix all five must pass.

Adding *rounds* inside one attempt does not help, and measurably hurt (24 rounds
crashed 2/5 where 8 rounds crashed 4/5). Once round 1 warms the child's
magazines with safe stacks, later rounds reuse the same ones; the outcome is
decided in round 1. Lowering `runtime_kthreads` to 2 narrows the magazine spread
and is what took it to 5/5.

## What fixes it

Nothing local. Raising `STACK_POOL_ENTRIES` moves the cliff; it does not remove
it. The fix is that a LibOS mapping made after an address space is cloned must
reach that address space — `docs/fork-implementation-plan.md`, Phases 1-3. When
it lands, this test should pass with the pool shrunk to a single stack, which is
the real assertion: **the pool becomes an optimisation rather than a
correctness requirement.**
