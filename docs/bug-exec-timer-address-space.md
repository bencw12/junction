# Bug: a timer for an exec'd process faults in the wrong address space

**Status:** root-caused and fixed. Reproducer: `docs/traces/fork_exec_sleep.c`.

**This is a defect in the fork/multi-address-space work**, not pre-existing
Junction behaviour. Before it, `fork()` returned `-ENOSYS`
(`--no_mas`: *"per-guest address spaces disabled; fork() will report ENOSYS"*),
so there was never a second process or a second address space and nothing could
reach this path.

## Reproduce

```c
pid_t c = fork();
if (c == 0) { execl("/bin/sleep", "sleep", "1", NULL); _exit(127); }
waitpid(c, &st, 0);
```

or, equivalently, from a shell:

```sh
sh -c 'sleep 1 & wait'
```

```
[  1.045495] fault with preemption disabled: addr=4feffddb4068
             rsp=5200021f9f80 pid=2 as=0 want_as=0
Aborting on signal: signal delivered while preemption is disabled
```

Deterministic: same fault address every run, 3/3 and 3/3 for the shell form.

## What is and is not required

Narrowed by bisection, each line one test:

| variant | result |
| --- | --- |
| `fork` + child `nanosleep(1)`, **no exec** | ok |
| `fork` + child `exec /bin/true` (exits at once) | ok |
| `fork` + child `exec /bin/sleep 1` | **crash** |
| `sh -c '( : ) & wait'` (fork, no exec) | ok |
| `sh -c 'sleep 0.2; echo done'` (foreground) | ok |
| `sh -c 'sleep 1 & wait'` (background) | **crash** |

So it needs all three: a **fork**, an **exec**, and the exec'd process
**outliving the call and holding a timer**. Neither fork nor exec alone does it,
and a sleeping child that never exec'd is fine.

## It is the timer expiry

The fault time tracks the sleep duration exactly:

| exec'd | fault at |
| --- | --- |
| `sleep 0.3` | 0.345 s |
| `sleep 1` | 1.045-1.048 s |
| `sleep 3` | 3.044 s |

So the fault is in the wakeup, not in `fork`, `exec` or the sleep syscall.

`want_as == as == 0`, meaning Junction believes the loaded address space is the
right one, while `addr=0x4feffddb4068` lies in a guest range. Something reached
by the timer path still points into a guest's memory, and it is dereferenced
from a core bound to a different address space.

This is the same *class* as a hazard already known and partly addressed: the
design notes record that Junction ran syscall handlers on the guest's stack by
default, which put LibOS state such as a Caladan `timer_entry` into guest
memory, and that stack switching is force-enabled whenever the address-space
device is present (`junction.cc:230`, `if (mas_enabled_) stack_switching = true`).
Stack switching *is* on here, so either that fix is incomplete or `exec` moves
something the fix does not cover — most likely because `exec` replaces the
process's `MemoryMap` and thus its address space, while a timer registered
before or across that transition still refers to the old one.

## Not caused by the Phase 0a discriminator change

Checked directly rather than assumed: reverting `syscall/seccomp.cc` to the
previous `!preempt_enabled()` test and rebuilding reproduces the identical
fault at the identical address. Restored afterwards.

## Why the test suites miss it

`scripts/fork_test.sh` (46/46) and `scripts/test.sh` (39/39) both pass. The fork
tests fork heavily but the children exit promptly rather than exec'ing and then
sleeping, and the threading tests never combine a second process with a timer.

## Why this matters more than the pool bugs

The pool-exhaustion crashes need thousands of concurrent objects. This one needs
`sleep 1 &`. Any shell script that backgrounds an external command hits it, which
makes it the most reachable failure found so far.


## Root cause

`exec` released the address space containing the program it had just loaded.

1. `fork()` gives the child its own address space `X`; its `MemoryMap` has
   `as_handle_ = X` and `owns_as_ = true`.
2. `execve` builds a **new** `MemoryMap` via `MemoryMap::Create()`, which never
   assigns a handle — the field defaults to `kRootAddressSpace` (0).
3. The new image is loaded into whichever address space the core is bound to,
   which is `X`.
4. `p.FinishExec()` installs the new map; the old map's destructor runs and,
   because `owns_as_` is true, calls `ReleaseAddressSpace(X)` — **destroying the
   address space the new image lives in.**
5. The process now claims address space 0, so the scheduler binds the root
   address space to it.

Nothing fails yet, because the core is still bound to `X`. The fault needs the
process to be **descheduled and resumed**: on resume `jmp_thread_direct`
restores the thread's context from an address that only existed in `X`. Hence a
fault on the switch itself, with no frame to report from, and only when a second
runnable process exists to switch away to.

`MemoryMap::TransferAddressSpaceTo` already existed in `mm.h`, commented "Used
by exec(), which replaces a process's mappings but keeps it in the same address
space" — **and nothing called it.** The helper was written and never wired up.

## The fix

Four changes, each forced by a failure the previous one exposed:

1. **`exec.cc`: call `old_mm.TransferAddressSpaceTo(**mm)`** before installing
   the new map, so the successor inherits the handle and its ownership.
2. **`exec.cc`: restore the LibOS `fsbase`** before the old map is dropped. A
   guest syscall runs on the guest's TCB, so LibOS code touching thread-local
   storage between the handover and the jump into the new image would fault on
   freed memory.
3. **`mm.cc`: do not release a transferred address space**, and do not leave the
   core bound to an address space that *is* being released — the next
   `MemoryMap::Create` would `MAP_FIXED_NOREPLACE` into the dead process's
   still-loaded mm and collide with its stale mappings (`MM Panic: mappings are
   not in sync with kernel`).
4. **`as.{h,cc}` + `mm.cc`: `TryActivateAddressSpace()`.** After the handover the
   old map can outlive its successor — it is destroyed when its last reference
   drops, possibly after the new image has exited and released the space. The
   plain `ActivateAddressSpace` calls `syscall_exit(-1)` on failure; the
   fallible variant lets the destructor recognise that the address space is
   already gone, in which case its mappings went with it and only the
   reservation is left to free.

A fifth attempt — unmapping the old image at handover time — was tried and
reverted: `Thread::~Thread()` writes to guest memory on teardown
(`clear_child_tid`), so the mappings must outlive it.

## Related finding

`MemoryMap::shares_reservation_` is set in `MemoryMap::Fork` and **read
nowhere**. It was harmless only because forked maps always took the `owns_as_`
path, which returns before any unmapping. Anything that routes them down the
other path will unmap a reservation shared with the parent.

## Verified

```
fork + exec sleep, C reproducer      child exited 0   (3/3)
sh -c 'sleep 1 & wait; echo survived'  survived       (3/3)
sh -c 'echo>f; sleep 0.2; cat f'       hello
sh -c 'echo>f; ( cat f ) & wait'       hello
```

The remaining shell case — `( sleep 0.2; cat f ) & echo > f; wait`, where the
file is created *after* the fork — still fails with SIGSEGV. That is the
separate memfs extent bug (`docs/traces/memfs_race.c`), now confirmed reachable
from a two-line shell script.
