# Intermittent hang in the fork storm

`bench/fork/fork_stress` hangs under Junction in roughly one run in four. It is
a hang, not a wrong answer: the process stays alive, every kthread is parked,
and nothing ever progresses.

**Root cause found and fixed; a second bug was uncovered underneath it and is
still open.** Read the resolution at the end before the investigation notes.

## Reproducing

```
build/junction/junction_run caladan_test.config -- \
    bench/fork/fork_stress -t 4 -s 2 -f 32
```

Roughly 22% of runs never terminate (12 hangs in 54 runs, across several
builds). `-O`, which skips the semantics section and runs only the stress
section, still reproduces it -- so the hang lives in the stress section.

Every hang stops at the same line:

```
[ stress: simultaneous fan-out of 32 children ]
[ stress: 4 thread(s), 2 s, 1024 KB private region, 16 COW pages/child ]
                                    <- nothing after this, ever
```

Native Linux never hangs; this is Junction-specific.

## It needs concurrent forkers

Measured 8 runs per configuration, on the same machine:

Measured 8 runs per configuration, on the same machine. `-t` is the number of
threads forking concurrently, `-f` the width of the one-off fan-out that
precedes the storm.

| configuration | hangs |
| --- | --- |
| semantics section only (`-S`) | 0/8 |
| storm, 1 thread  (`-O -t 1 -f 32`) | 0/8 |
| storm, 2 threads (`-O -t 2 -f 32`) | 0/8 |
| storm, 4 threads (`-O -t 4 -f 32`) | 2/8 |
| storm, 4 threads, no fan-out (`-O -t 4 -f 1`) | 0/8 |
| storm, 8 threads, no fan-out (`-O -t 8 -f 1`) | 2/8 |

Two things are well supported by this. It is not the semantics tests, and it is
not forking as such -- it takes *several processes forking and exiting at the
same time*, and the more of them there are the more often it happens.

One thing is **not** supported, and is recorded here so it is not mistaken for a
result later: the difference between `-t 4 -f 32` (2/8) and `-t 4 -f 1` (0/8)
is far too small to conclude that the fan-out matters. Eight threads hang
without any fan-out at all, so the fan-out is certainly not *required*; whether
it contributes is unknown. Differences of this size need hundreds of runs, not
eight -- a lesson this investigation already learned the expensive way, by
briefly believing a 0-in-10 arm had found the root cause.

**The most economical repro** is therefore `-O -t 8 -s 2 -f 1`: no semantics
section, no fan-out, nothing but concurrent forking, and it still hangs.

Note that `CreateProcessFork` already serializes the clone itself under
`AddressSpaceForkLock()` -- exclusions are a property of the address space, so
two clones cannot be in flight at once. Whatever races here is therefore either
outside that lock, or between a fork and some *other* thread's child exiting.

## The state when it is hung

Every host thread is parked in Caladan's idle path:

```
    TID STAT WCHAN                        %CPU
2121970 SNl  ksched_ioctl                  9.2
2121971 SNl  ksched_ioctl                  7.9
      ... all 10 identical ...
```

That is worth reading carefully:

- **nothing is spinning**, so it is not a spin-lock deadlock;
- **nothing is blocked in a host system call** -- no thread sits in `wait4`,
  `futex` or anything else, so no kthread is stuck inside the fork path;
- every kthread parked means **every uthread is blocked and nothing is
  runnable**.

That is the signature of a lost wakeup: something is waiting for an event that
has already happened, or for one that will never come. The most likely
candidates are a parent blocked in `Process::DoWait` (`proc.cc:657`, waiting on
`child_waiters_`) whose child exited without the wakeup landing, or a child
thread that never became runnable. `SignalLocked` (`proc.h:859`) does the
`child_waiters_.WakeAll()` under `shared_sig_q_`, and `DoWait` re-checks after
waking (`proc.cc:676`), so the obvious race is already closed -- whatever this
is, it is subtler.

An earlier symptom of what is probably the same bug: one run reported
`no child reported a failure (2 failures)` out of 7095 forks, with no
`FAIL(child)` line anywhere. No child printed a failure, so the checksum check
passed; the failures came from children that did not exit normally.

## What has been ruled out

Each arm below was run interleaved with an unmodified build on the same machine,
so load and machine state are shared.

| arm | hangs |
| --- | --- |
| unmodified | 7/32 |
| Phase 1 fault-repair hook removed from the page-fault path | 4/10 |
| Phase 0b frozen-invariant check compiled out | 0/10 |
| Phase 0b frozen-invariant check made lock-free | 1/12 |

Neither of the Phase 0/1 additions accounts for it. The third row looked
decisive and is not: at the pooled 22% rate, 0 hangs in 10 runs happens 8% of
the time by chance, and the fourth row -- which keeps the check but removes the
global lock it was taking -- still hangs.

Also ruled out:

- **Not the logging path.** No hung run had logged a `FROZEN VIOLATION`, so it
  is not a `LOG()` call deadlocking in signal context.
- **Not `--debug_as_audit` or `--debug_frozen_probe`.** Neither flag is set in
  these runs.
- `--no_mas` is not a usable control: it makes `fork()` return `ENOSYS`, so the
  test fails immediately instead of exercising the path.

## Why it is probably not new

Nothing in Phase 0 or Phase 1 touches process exit, `wait`, or thread wakeup.
The changes that *are* in the fork path -- the frozen-invariant check on unmap,
and the fault-repair hook -- have each been removed and measured, and neither
moves the rate. What today's work plausibly did is perturb timing.

This is not the same as proving the hang predates the work. A build with none of
the multi-address-space changes would settle it, and does not exist: the whole
feature is uncommitted, so there is no baseline to check out. Building one is
the first thing to do next.

## What to do next

1. **Get a thread dump.** The single most useful thing missing. Everything above
   is inferred from host-thread state, because there is no way to ask a running
   Junction which uthreads exist and what each is blocked on. A dump on
   `SIGUSR2` -- walking each `Process`'s `thread_map_` and printing each
   thread's wait state -- would turn this from inference into observation, and
   would pay for itself the next time something hangs.
2. **Narrow the repro further.** Concurrency is established as necessary; the
   remaining questions are the minimum number of forking threads and whether
   the 32-child fan-out that precedes the storm contributes.
3. **Build a pre-MAS baseline** to settle whether the hang predates this work.


---

# Resolution

## The hang: an unbalanced credit in `prepare_interruptible`

Caladan's interruptible wait is a refcount in `thread->interrupt_state`
(`lib/caladan/inc/runtime/interruptible_wait.h`):

| operation | effect |
| --- | --- |
| `prepare_interruptible` (arm) | `+WAKER_VAL` (65), reports whether an interrupt was already pending |
| `deliver_interrupt` (signal) | `+1`, readies the thread if the prior value was non-zero |
| `interruptible_wake` (waker) | `-65`, readies **only if the result is zero** |

The arm added its 65 *unconditionally* and only reported the pending interrupt.
Every caller returns early on that report -- Junction's `WaitInterruptible`
(`bindings/sync.h`), `runtime/timer.c:300`, `runtime/net/waitq.h:80` -- without
parking and without registering with any waker. The credit it just added is
therefore one that nobody will ever subtract.

Accumulate two and `interrupt_state` exceeds `2 * WAKER_VAL`. From then on a
waker's subtraction can never reach zero, so `thread_ready()` is never called:
**the thread is wakeable only by interrupts**, and hangs for good once they
stop. In the storm that is the instant the last child is reaped.

Observed directly: `istate=131` (= 65 + 65 + 1) on a thread parked in
`FutexTable::Wait` whose waiter had *already been dequeued* -- a `FUTEX_WAKE`
that ran and did nothing.

The fix is to make arming idempotent: add the credit only when `PREPARED_FLAG`
is clear, so at most one is ever outstanding.

Measured over three batches, including 40 interleaved pairs on the same machine
so load and machine state are shared:

```
                        hangs         assertions
pre-fix baseline        18/80  (22.5%)   0/80
idempotent arm           0/120 (0%)      2/120 (1.7%)
```

The hang result is unambiguous. The assertion is discussed below.

Note this is **not** a multi-address-space bug. It is in Caladan's wait
primitive, exposed by the SIGCHLD storm that thousands of concurrent forks
generate.

### Two fixes that look right and are not

Both were implemented and measured, because the reasoning that rejects them is
not obvious from the code:

- **Subtract the credit after an early return.** Fails immediately with
  `BUG_ON(th->thread_ready)`.
- **Decline to arm when an interrupt is pending.** Removed every hang (0/30)
  but produced 4/30 assertion failures.

Both clear `PREPARED_FLAG`, and the flag is not bookkeeping -- it is a protocol
signal. `interruptible_wake_test()` readies the thread *unconditionally* when
the flag is clear, so clearing it lets a waker ready a thread that is already
running. The invariant is "at most one outstanding credit", not "none".

## Still open: `BUG_ON(th->thread_ready)`, ~1.7% of runs

A rare double-ready. **Not attributable to the fix on the evidence available:**
2 in 120 fixed runs against 0 in 80 baseline runs is p ~ 0.52. An earlier
reading of 2/40 against 0/40 looked like the fix had introduced it; it had not,
and a following batch of 40 produced none at all. Telling a 1.7% event apart
from zero needs several hundred runs per arm.

It is a real defect either way, and the mechanism is measured rather than
inferred. At the moment of failure:

```
double ready: istate=0 running=0 in_syscall=1 link_armed=0
```

`interrupt_state` is zero, which rules out `deliver_interrupt` -- that only
readies when the prior value is non-zero. So it is a waker taking the
unconditional branch of `interruptible_wake_test()`:

```c
static inline bool interruptible_wake_test(thread_t *th)
{
	return !check_prepared(th) ||
	        atomic8_sub_and_fetch_relaxed(&th->interrupt_state, WAKER_VAL) == 0;
}
```

So the tree currently trades a 23% silent hang for a 5% loud abort. That is not
a good enough resting place, though a `BUG_ON` is more debuggable than a hang.

### Does it need fork, or just signals racing wakers?

`docs/traces/signal_futex_storm.c` strips the fork out: one process, never
cloned, so `MultipleAddressSpacesExist()` stays false and every
multi-address-space path is inert. Workers block in `pthread_cond_wait` (a futex
wait underneath) while a signaller sprays `SIGUSR1` with `pthread_kill` and a
waker broadcasts continuously -- signals racing wakers, which is what the
protocol is about.

```
no-fork workload   0 asserts in 600s  (20 runs x 30s, ~23.5M wakeups)
fork storm         2 asserts in ~480s (120 runs x ~4s of storm)
```

The fork-free workload drives the primitive harder -- ~39k wakeups/s against
maybe 10-20k interruptible waits/s in the storm -- and produced nothing. At the
storm's rate, 600s predicts ~2.5 assertions; observing zero has probability
~8%. Moderate evidence, not proof, that the primitive alone is not sufficient
and that something about process creation or exit is involved.

**What cannot be tested from this tree.** The next differentiator would be many
processes *without* address-space cloning, and that does not exist here: every
process creation goes through `CreateProcessFork` -> `CloneCurrentAddressSpace`,
`posix_spawn` and `vfork` included. Separating "process exits deliver SIGCHLD
through the shared queue" from "address spaces are being cloned" requires a tree
that predates the multi-address-space work. That is a revert in a scratch
worktree, which is cheap once this work is committed and expensive while it is
not.

The underlying flaw is the mirror of the arm-side bug: **`interrupt_state == 0`
is ambiguous.** It means both "never armed" and "armed, and already woken". A
second waker arriving before the thread runs cannot tell the difference, and
readies a thread that is already queued.

**Next step, and the trap in it.** The wake side needs the same treatment as
the arm side -- a waker must not call `thread_ready()` on a thread it has not
established a claim on. But note that simply switching `rt::ThreadReady` to the
other variant does not work:

```c
interruptible_wake(th)           /* guesses via check_prepared: double-ready when the credit is gone */
interruptible_wake_prepared(th)  /* assumes a credit: subtracts to -WAKER_VAL, a LOST wakeup when it is not */
```

That trades the abort back for the hang. The claim has to be per-waker, the way
`ThreadWaker::Wake()` already does it by atomically exchanging `th_` so only one
caller can win. `thread_ready_prepare()` now logs `interrupt_state` and the
three nearest return addresses before aborting, so the next occurrence names
the second waker: reproduce with `-O -t 8 -s 2 -f 1` and resolve the addresses
against the text anchor.


---

# Follow-up: the double ready is fork-free, pre-existing, and now deterministic

The question was whether `BUG_ON(th->thread_ready)` needs the multi-address-space
work at all. It does not, and the way to show it was not a pre-MAS build (plain
`fork()` did not exist before this work -- `DoClone` returned `-ENOSYS` for
anything but vfork or a thread -- so "the fork storm without MAS" is not a
runnable configuration). It was to reproduce the failure with a workload that
never clones an address space.

`docs/traces/thread_churn_storm.c` does that: N threads each loop
`pthread_create` / `pthread_join` of a trivial thread while a signaller sprays
`SIGUSR1` at them. Thread exit and join drive exactly the code the fork storm
hangs between -- `Thread::~Thread()` clearing `child_tid` and calling
`FutexTable::Wake`, and the join's futex wait -- with `MultipleAddressSpacesExist()`
false throughout.

| workload | current build | pre-fix baseline |
| --- | --- | --- |
| `3 2 1` (2 churners + signals) | guest `SIGABRT`, exit 134, within 0.1 s, 3/3 | exit 134, 4/4 |
| `3 2 1 1` (same, `SA_RESTART`) | `BUG_ON(th->thread_ready)`, 3/3 | assertion, 3/3 |
| `3 1 1` (1 churner) | survives, but 82 threads/2 s instead of 166k | -- |
| `3 8 0` (8 churners, no signals) | fine, 130k threads/2 s | -- |

Both failures are therefore pre-existing and independent of address-space
cloning. They are also, at last, deterministic.

## The double ready: both wakers named

`thread_t` now records who last made a thread runnable (`last_ready_ra`,
`last_ready_istate`), and `thread_ready()` logs both callers on a double ready.
Across three runs the pairs were:

```
FIRST  WaitQueue::WakeAll          SECOND  Process::SignalThread
FIRST  Process::SignalThread       SECOND  FutexTable::Wake
FIRST  Process::SignalThread       SECOND  Process::SignalThread     (istate 66 both times)
```

`Process::SignalThread` is the common element. It reaches `thread_ready()`
through `SendIpi()` -> `deliver_interrupt()`, which readies whenever the prior
`interrupt_state` is non-zero. That condition is "armed", not "armed *and
parked*": the state was observed as `istate=66 running=1` -- a thread with a
live arm credit that is executing on a CPU -- and as a thread already queued
from a previous ready. `deliver_interrupt()`'s contract ("can only be called
once") is not being upheld under a `pthread_kill` storm, and each extra call
lands `thread_ready()` on a thread that is not parked.

## The guest abort: a restart code leaks to userspace

Right before `abort()` the guest's strace shows:

```
[1:3] munmap(0x4fffe9db2000, 67112960) = -512      -ERESTARTSYS
[1:3] --- SIGUSR1 ---
```

`-512` must never reach a program. glibc's `pthread_join` frees the dead
thread's stack and treats any `munmap` failure as fatal, silently
(`nptl/nptl-stack.c:83`: `if (__munmap(...) != 0) abort();`).

The path: `MemoryMap::MUnmap` takes `mu_` with `InterruptOrLock`;
`SharedMutex::InterruptibleLock()` goes through `WaitInterruptibleNoRecheck()`
when contended, which bails if an interrupt is pending; `MUnmap` returns
`EINTR`; `usys_munmap` converts it with `MakeCErrorRestartSys` to
`-ERESTARTSYS`, relying on the syscall-exit path to restart the call or turn it
into `-EINTR`. It did neither. `interrupt_state` was measured as 66 at that
point, so the `entry.S` gate (`jg` on the signed byte) does run `RunSignals()`;
why the fix-up does not reach the returned `rax` is the open question -- the
frame `CheckRestartSysPostHandler` writes and the frame the unwind restores are
the place to look.

## Other observations, recorded but not chased

- the vfork storm (`docs/traces/vfork_signal_storm.c`) is not a hang: with the
  watchdog on it completed, 276,899 processes in 5 s against 1.6M in 3 s on
  another run. The waiter and main threads are being starved; whether it
  finishes inside a timeout is luck;
- after the guest exited with 134, `junction_run` itself sometimes stayed alive
  until the timeout (2 of 4 baseline runs). Unexplained.

## Instrumentation left in

- `thread_t::last_ready_ra` / `last_ready_istate`, set in `thread_ready()` and
  `thread_ready_head()`, printed by `thread_ready_prepare()` on a double ready.
  Only `__builtin_return_address(0)`: deeper levels walk frame pointers `-O3`
  does not keep, and an earlier version of this diagnostic faulted on them
  before it could print -- which hid the very thing it was built to catch;
- `usys_munmap` warns (at most 8 times) when it is about to hand a restart
  code back, with the `interrupt_state` the exit gate will see.


---

# Status: hang fixed, guest SIGABRT fixed, double-ready still open

Three failures were found under fork/thread churn. Two are fixed; the third is
one bug with two triggers and is left open with a precise root cause, because
every point-fix tried so far only moves the symptom.

## Fixed 1 -- the hang (committed, lib/caladan idempotent arm)

The leaked wake credit in `prepare_interruptible`. 18/80 -> 0/120. See the
earlier sections; unchanged.

## Fixed 2 -- guest SIGABRT during pthread_join (this change, junction mm.cc)

`MemoryMap::{SetBreak,MMap,MProtect,MUnmap,MAdvise}` took the map lock with
`InterruptOrLock`, which returns EINTR when a signal is pending and the lock is
contended; `usys_*` turned that into `-ERESTARTSYS`. But Linux never fails these
calls with EINTR, and glibc's `pthread_join` frees the joined stack with
`munmap()` and calls `abort()` on *any* failure (nptl/nptl-stack.c:83). Plain
locks at all five sites. Deterministic repro `thread_churn_storm 3 2 1`: was
SIGABRT 3/3, now clean 5/5. This is independent of the double-ready below.

## Open -- the double ready (`BUG_ON(th->thread_ready)`)

One probabilistic bug, surfacing through more than one workload. Measured
rates (small samples, heavy machine-load dependence -- treat as order of
magnitude, not precise):

- `thread_churn_storm 3 2 1 1` (SA_RESTART): ~1 in 8;
- the fork storm (`-O -t 8 -s 2 -f 1`): ~8 in 60.

An earlier note in this session called the churn case "deterministic, 3/3";
that was small-sample luck. It is the same probabilistic double-ready.

Mechanism, from a per-thread event ring dumped at the assertion (tags:
`A`=arm, `I`=deliver_interrupt readies, `R`/`r`=thread_ready() call
(lowercase = target already ready), `C`=scheduler dispatch clears
thread_ready). A representative capture:

```
A(65) R C   I(66) R C   I(66) R C   I(66) R C   A(65) R(0x17) r(0x17) C
                                                        ^^^^^^^^^^^^^^^ the double
```

Established:

- both readies of the doubled thread come from the **same caller and the same
  queue instance** -- `WaitQueue::WakeAll` on pid 1's `child_waiters_`
  (`q1 == q2`, `FIRST` low byte == `SECOND` low byte == 0x17);
- the interrupt-wake cycles (`I` then `R`) show the load-bearing fact:
  **`deliver_interrupt()` readies a thread without removing it from its
  `WaitQueue`.** It is a Caladan primitive and does not know about the
  bindings-layer `WaitQueue`. So a thread that is interrupt-woken while armed on
  `child_waiters_` stays linked, `thread_ready=true`, until it later runs and
  disarms;
- a `WakeAll` (from a concurrent SIGCHLD) landing in that window pops the
  still-linked thread and readies it a second time -> `BUG_ON(th->thread_ready)`.

So the two wakers do not coordinate removal-before-ready across the two
mechanisms. `interruptible_wake_test()`'s refcount handles the case where the
credit is still present (`istate=66`, flag set -> the subtraction is non-zero,
no second ready); the hole is the `istate=0` / flag-clear branch, which readies
unconditionally.

Not yet closed: the exact step that leaves the doubled thread with `istate=0`
(no credit, flag clear) while still reachable on `child_waiters_`. `istate=0`
implies it was armed without `prepare_interruptible` (a plain `Wait`), yet
`DoWait` uses `WaitInterruptible` (which prepares). Reconciling those two is the
remaining question, and it is where the fix has to be designed -- probably by
making `deliver_interrupt` (or the WaitQueue arm) keep the queue membership and
the readiness in agreement, so a thread is never both `thread_ready` and
poppable by another waker.

A hypothesis from an earlier revision -- that `reset_interruptible_state()` /
`set_interrupt_state_interrupted()` clobber a live `WAKER_VAL` credit -- was
tested (log every such write landing on a thread with `PREPARED_FLAG` set) and
**refuted**: 324,585 such writes over 30 runs, all on threads with
`armed=0 ready=0` (the normal post-wake reset), none on an armed or ready
thread.

Why no point-fix was shipped -- each was measured and each only moves it:

All rates below were measured close together on the same machine; earlier
cross-session comparisons (e.g. "2/120") are not reliable, machine load moves
the rate several-fold.

| change | churn SA_RESTART | fork-storm assert | fork-storm hang |
| --- | --- | --- | --- |
| munmap only (shipped) | ~1/8 | ~8/60 | 0 |
| + deliver_interrupt readies only on WAKER_VAL | ~0/5 | ~6/60 | 0 |
| + interruptible_park sticky bit | 0 | 0 | **~7/60** (lost wakeup) |

`deliver_interrupt` (6/60) vs without it (8/60) is statistically
indistinguishable (Fisher p ~ 0.8): it does not measurably change the fork
storm, and its apparent churn win is within noise, so it is not shipped.
`interruptible_park` does eliminate the assert (0/60, significant) but cannot
tell "already readied" from "credit reset, still parked", so it drops real
wakeups -- the assert becomes a hang. Neither is a fix; both only move the
symptom. The next diagnostic step is to capture both wakers' return addresses
*with ASLR off* (setarch -R) on an asserting run so they resolve, and determine
how the doubled thread remains reachable by a second waker after the first ready
dequeued it -- that is the unknown, and no fix should be attempted before it is
answered.

The correct fix is at the accounting level: no code path may *overwrite*
`interrupt_state` while a thread holds an arm credit; `reset`/`set-interrupted`
must be add/sub that preserve `WAKER_VAL`, or must run only when the thread is
provably not armed. That is an invasive change to the signal subsystem and wants
a focused pass with the invariants written down, not a storm-rate bisection.

The lightweight diagnostic in `thread_ready_prepare()` (first-waker capture) is
left in to make the next occurrence self-describing.

