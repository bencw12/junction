# Multiple address spaces, and fork()

Junction runs every guest process inside the address space of a single Linux
process. One userspace scheduler multiplexes them all, which is what makes
Junction's system calls cheap — but it has a hard consequence:

> Two guests can never occupy the same virtual addresses, so `fork()` — which by
> definition hands the child its parent's addresses — cannot be implemented.

Before this work `fork()` returned `-ENOSYS`. Only `vfork()` and thread
creation were supported, because both share the caller's address space.

This document describes how guests were given address spaces of their own, and
what that costs.

## Where the idea comes from

Dune let an unprivileged process own its page tables outright — VT-x, ring 0,
its own `CR3` — so a library OS could create and switch address spaces itself.
We take the same division of labour but leave Linux's `mm_struct` as the
page-table implementation. That choice is the reason the resulting `fork()` has
*identical* semantics rather than merely similar ones: copy-on-write, reverse
mapping, reclaim, and accounting are all still Linux's own code, exercised the
same way an ordinary `fork()` exercises them.

What Linux does not offer userspace is the one operation the design needs:

> let a thread bind itself to a different `mm_struct`.

That, and nothing else, is what `kern/junction_as.c` adds.

## How an address space is created

Junction does not construct an address space; it asks Linux to duplicate one.

1. Mark every *other* guest's memory `MADV_DONTFORK`, so it is left out.
2. `clone()` the host process. Linux performs the copy-on-write duplication.
3. Ask the module to take a reference to the clone's `mm`, which returns a
   handle.
4. Kill the clone. The address space outlives it.

The clone never runs anything but `pause()`, and it starts on a private scratch
stack (`junction_as_clone_parked` in `kernel/ksys.S`), because Junction's own
stacks are shared memory — a clone left running on the caller's stack would
overwrite the caller's live frames.

`MADV_DONTFORK` applies to the whole address space, so cloning is serialized by
a single lock (`AddressSpaceForkLock`).

## How a core switches address space

Caladan already called `on_sched(thread_t *)` just before jumping into a
uthread. Junction's `on_sched` now binds the core to that guest's address space
first.

The bound handle is cached in Caladan's **per-kthread area**, addressed through
`%gs` — not in thread-local storage, because `%fs` has already been repointed at
the guest's TLS by the time `on_sched` runs. Threads of one process share a
handle, so switching between them costs a comparison; only moving a core between
*processes* reaches the kernel.

Handles carry a generation counter in their upper bits and are never reused.
Without that, a core still cached as "bound to handle 2" would skip the switch
when a freshly created address space was handed the recycled id 2 — and go on
running a guest on a dead address space.

## The invariant that makes it work

**All of the LibOS's writable memory must be `MAP_SHARED`.**

This is not only about keeping data structures coherent. A core keeps running on
its own stack across a switch, so a private stack would lose every frame pushed
in the other address space, and the `ret` after the switch would jump into
whatever the other copy happened to contain.

Three things enforce it:

- Caladan's runtime memory, uthread stacks, and kthread stacks are mapped
  `MAP_SHARED` (`cfg_shared_runtime_mem`). glibc places a thread's static TLS at
  the top of a caller-supplied stack, so giving kthreads explicit shared stacks
  makes their TLS shared too.
- At startup, `InitAddressSpaces()` rewrites every remaining private writable
  mapping — Junction's `.data`/`.bss`, the heap, the shared libraries' data —
  into a shared mapping with identical contents, by pushing the bytes through a
  `memfd` and mapping it back over the same addresses. glibc is then pinned down
  (`M_ARENA_MAX=1`, no trimming, no `mmap` for large chunks) and the heap grown
  once up front, so it cannot sprout new private mappings afterwards.
- `AuditLibOSMemory()` re-reads `/proc/self/maps` and reports anything still
  private and writable. It runs at startup, where a violation is a loud warning
  rather than a mystery later.

### Anything the LibOS allocates after a clone is invisible to that clone

A mapping created after an address space has been cloned exists only in the
address space that created it. This bit us twice: a newly allocated Caladan
stack, and a newly allocated slab of runtime heap, each faulted in a guest that
had been forked earlier.

Both pools are therefore reserved **once, up front**, as a single shared
mapping each (`STACK_POOL_ENTRIES` stacks, `LGPAGE_POOL_ENTRIES` large pages).
The reservations are sparse — `MAP_NORESERVE` — so they cost address space, not
memory: Junction's resident footprint is unchanged, and the VMA count actually
*fell* from 771 to 128, because 640 individual stack mappings collapsed into
one. Since cloning copies per-VMA, that makes forks cheaper, not more expensive.

Exceeding either pool logs a warning and falls back to the old behaviour.

## System call handlers must not run on guest stacks

Junction normally runs a guest's system call handler on the guest's own stack.
With one address space that is free; with several it is a correctness bug,
because anything the LibOS leaves there lives in *that guest's* memory. A
Caladan `timer_entry` for `nanosleep()` is the case that found this: the timer
softirq is not a Junction thread, so it runs in whatever address space its core
last had, and dereferenced the entry from the wrong one.

Stack switching is therefore enabled automatically whenever the address-space
device is present.

## What fork() does

`DoClone()` treats a clone that shares *nothing* with its caller as a fork.
glibc has not issued the `fork` system call in twenty years — `fork()` is
`clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD)` — so the test is on the
flags, not the call number. The raw `fork` system call is implemented too, for
programs that call it directly.

`CLONE_CHILD_SETTID` asks for the child's tid to be written into the child's
memory, which does not exist yet and is unreachable from the parent once it
does. Junction writes the value into its own memory first and lets the clone
inherit it, then puts its own value back; only the calling thread can see that
location, so the window is harmless.

The child gets a copied file table, the parent's signal dispositions and
limits, a reset itimer, and no pending signals — and a `MemoryMap` that shares
the parent's reserved range (reference counted, since both now use those
addresses) while owning the new address space.

## Two changes to existing behaviour

**Shared anonymous memory is now really shared.** `usys_mmap` used to downgrade
`MAP_SHARED|MAP_ANONYMOUS` to private — invisible when there is only one address
space, and exactly wrong once a process can fork.

**A process killed by a signal reports as killed.** Junction turned a fatal
signal into `exit_group(128 + signo)`, so `waitpid()` reported `WIFEXITED` with
code 143 instead of `WIFSIGNALED` with `SIGTERM`. Callers branch on that
difference.

## Building and running

```
make -C kern                 # builds junction_as.ko and its standalone test
sudo insmod kern/junction_as.ko
kern/as_test                 # 13 checks: isolation, sharing, teardown, latency
```

`scripts/build.sh` then builds Junction as usual.

## Turning it off

Per-guest address spaces are used whenever `/dev/junction_as` is present. They
are switched off either by not loading the module, or explicitly:

```
junction_run <config> --no_mas -- <program>
```

`--no_mas` is a single switch over all three layers, decided before the Caladan
runtime starts:

| | default | `--no_mas`, or no module |
| --- | --- | --- |
| guest address spaces | one per process | one, shared (`fork()` → `ENOSYS`) |
| LibOS memory | `MAP_SHARED`, pooled stacks and large pages | private, a mapping per stack, with guard pages |
| system call handlers | on Junction's stacks | wherever `--stackswitch` says |

That matters for measurement as much as for safety: comparing against Junction
with only the module unloaded still leaves the shared-memory changes in place,
so `--no_mas` is the honest baseline.

`kern/as_test` is worth running first — it exercises the module on its own, and
it is what should be run inside a VM after any change to it.

## Verifying

```
make -C bench/fork
build/junction/junction_run build/junction/caladan_test.config -- \
    bench/fork/fork_stress            # semantics + stress
build/junction/junction_run build/junction/caladan_test.config -- \
    bench/fork/fork_latency           # fork -> first instruction in the child
scripts/as_demo.sh                    # shows cores in different address spaces
```

`bench/fork/README.md` describes what each one measures.

## Results

Measured on a 56-core Xeon Gold 5420+ (Sapphire Rapids), Linux 6.8.0-138,
Junction with 10 kthreads. Native numbers are the same binaries run outside a
container, on the same machine.

### Semantics

`bench/fork/fork_stress` runs 46 checks. Native Linux passes 46; Junction
passes 46, repeatably.

### fork() to the child's first instruction

Median, in microseconds, as the parent's dirty private footprint grows:

| parent footprint | Linux | Junction |
| ---------------- | ----- | -------- |
| 0 MB             |  73.3 |    142.2 |
| 16 MB            | 213.1 |    236.4 |
| 64 MB            | 642.4 |    530.9 |

Junction carries about 70 us of fixed overhead — a host `clone()`, the adopt
ioctl, killing and reaping the clone, and building the guest's process and
thread objects. Its marginal cost is lower than Linux's (6.1 vs 8.9 us per MB),
which is why it overtakes at larger footprints: the child is started on a core
the userspace scheduler already holds, rather than waiting for Linux to
schedule a fresh task.

That both curves rise with the footprint at all is the point. Fork latency
grows with the number of pages the parent has dirtied because that is what
copying page tables costs; an implementation that was not really doing
copy-on-write would be flat.

### Throughput

fork + exit + wait of a small process, per second:

| forking threads | Linux  | Junction |
| --------------- | ------ | -------- |
| 1               |  8,032 |    2,748 |
| 2               | 13,128 |    3,824 |
| 4               | 13,234 |    3,738 |
| 8               | 11,445 |    3,943 |

Linux peaks near 13k/s and then declines: `fork()` takes the parent's
`mmap_lock` for write, so threads of one process serialize against each other.
Junction plateaus around 3.8k/s for a different reason — `AddressSpaceForkLock`
serializes cloning globally, because `MADV_DONTFORK` is a property of the
address space rather than of a call.

### What address spaces cost the scheduler

Context switch latency. The `--no_mas` column is the honest baseline:
unloading the module alone would leave the shared-memory changes in place.

| | default | `--no_mas` | native Linux |
| --- | --- | --- | --- |
| `sched_yield`, 2 threads    |   29 ns |  26 ns |  132 ns |
| `sched_yield`, 8 threads    |   18 ns |  15 ns |   35 ns |
| futex round trip, 1 process |  409 ns | 398 ns | 6562 ns |
| futex round trip, 2 processes | 1666 ns | n/a  | 6362 ns |

Threads of one process are unaffected: the cached handle matches and the
comparison short-circuits before any system call. Moving a core between
processes costs about 564 ns per hand-off, which is the switch ioctl — `as_test`
measures a bare switch at 522 ns. Even paying it, a cross-process hand-off under
Junction is four times faster than the same thing on Linux.

### Bugs this found in Junction

**A bad pointer to `wait4()` took down the container.** `DoWait()` wrote the
exit status into the caller's memory while holding `shared_sig_q_`. A spinlock
means preemption is off, and Junction cannot recover from a fault there, so it
aborted — where Linux simply returns `EFAULT`. It surfaced from a one-line test
program that called `wait(pid)` instead of `wait(&status)`: Linux quietly
tolerates the mistake, Junction died of it. The writes now happen after the
lock is dropped, and a bad pointer is rejected against the guest's own VMA
table.

Matching Linux exactly here meant reproducing a wart: Linux reaps the child and
*then* copies the status out, so a bad pointer loses the exit status for good
and a second wait reports `ECHILD`. Junction now does the same.

### A deadlock this found in Junction

`Process::ForEachProcess()` dropped a `shared_ptr<Process>` while holding
`pid_map_lock_`. If that was the last reference, `~Process` ran and took the
same non-recursive spinlock to deregister itself. Nothing hit it before because
processes rarely died while the map was being walked; a fork storm hits it
within seconds.

## Known limitations

- **UINTR must be disabled** (`scripts/setup_machine.sh nouintr`). Creating an
  address space creates a short-lived host task, and ksched's `sched_switch`
  hook unloads the core's UINTR state whenever it runs a task that is not the
  core's assigned kthread. `SENDUIPI` then raises `#UD`. Junction falls back to
  signals for preemption. Fixing this properly means either teaching ksched
  about transient tasks, or creating address spaces without a task at all by
  calling `dup_mmap()` from the module.
- **Pooled stacks have no guard page.** Turning one into `PROT_NONE` splits the
  pool's mapping, and cloning copies every mapping, so guards would make fork
  slow down with every stack ever allocated.
- **Cloning is globally serialized**, because `MADV_DONTFORK` is a property of
  the address space.
- **Shared file mappings** are still unimplemented; only anonymous ones are.
- **`getrusage` reports all time as user time.** Junction does not separate time
  spent in the guest from time spent in the LibOS on the guest's behalf.
