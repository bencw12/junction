# What makes fork() hard in a single-address-space library OS

Notes from implementing `fork()` in Junction. Most of these were not visible
from the design; they were found by running into them. They are recorded here
because almost all of them generalize to any library OS that multiplexes guests
inside one host process.

The shape of the whole problem: Junction's performance comes from putting every
guest in one address space, and `fork()`'s entire semantics come from giving a
child the same addresses as its parent. Those two facts are in direct
opposition, and everything below is a consequence.

---

## 1. The single-address-space assumption is not localized

It would be convenient if "one address space" were a property of the memory
subsystem. It is not; it is an assumption that has diffused into the design.

Junction gives each guest a private 64 GB slice of one address space and
relocates position-independent binaries into it. That is not an implementation
detail that `fork()` can work around — it is the reason `fork()` was returning
`ENOSYS`. A forked child must see its parent's memory at its parent's
addresses, and a second guest cannot be at those addresses.

**The lesson**: adding real address spaces is not a feature addition to the
memory subsystem. It changes what "a process" means, and the code that has to
change is spread across the scheduler, the system call path, the signal path
and the allocator.

## 2. The LibOS's memory must be shared — and stacks make that a control-flow requirement

The obvious requirement is that LibOS *data structures* stay coherent: if the
process table lives in memory that was copied rather than shared, two address
spaces disagree about which processes exist.

The non-obvious requirement is stacks. A core that switches address space keeps
executing on the same stack across the switch. If that stack were private, the
frames pushed while running in one address space would not be there after
switching back, and the `ret` after the switch would jump into whatever the
other copy of that stack happened to contain. The stack is not data the LibOS
reads; it is where its control flow lives.

The same applies to thread-local storage, which glibc places at the top of a
thread's stack — so making kthread stacks shared is what makes their TLS shared.

**The lesson**: "which memory must be shared" is not answered by auditing data
structures. Anything the LibOS *executes on* counts too.

## 3. System call handlers running on guest stacks put LibOS state in guest memory

Junction runs a guest's system call handler on the guest's own stack by default,
which costs nothing when there is one address space.

With several, it is a correctness bug, and a subtle one: anything the LibOS
leaves on that stack lives in *that guest's* memory. The case that found this
was a Caladan `timer_entry` for `nanosleep()` — a LibOS object, allocated on
the caller's stack, whose address is then stored in a per-core timer heap. When
the timer fired, the softirq dereferenced it from whatever address space its
core happened to be bound to, and called a function pointer read out of another
process's stack.

**The lesson**: a library OS that borrows the guest's stack has, in effect, put
part of its own state in guest memory. That is invisible until guest memory
stops being universal.

## 4. Not every thread goes through the scheduler hook

The address-space switch hangs off Caladan's `on_sched()` hook, which fires when
a *Junction* thread is scheduled. Caladan's own internal uthreads — timer
softirq, iokernel softirq — are not Junction threads and do not fire it. They
run in whatever address space their core last had.

That is fine as long as they only touch LibOS memory. It stops being fine the
moment any LibOS state has leaked into guest memory (see above). The two bugs
compound: one puts LibOS state somewhere per-address-space, the other runs LibOS
code without fixing the address space.

**The lesson**: enumerate every context that can run LibOS code, not just the
ones the feature was designed around.

## 5. Identifiers that are recycled become stale bindings

The address-space switch is only cheap because each core caches which address
space it is bound to and skips the switch when the handle matches. The kernel
module handed out handles from an IDR, which reuses freed ids.

So a core cached as "bound to handle 2" would skip the switch when a freshly
created address space was handed the recycled id 2 — and keep running a guest on
a *dead* address space. It reproduced only under load, only with more than one
core, and presented as a guest jumping to a nonsense instruction pointer.

Handles now carry a generation counter and are never reused.

**The lesson**: any cache of "am I already in the right state?" turns identifier
reuse into a correctness bug. This is worth checking for by construction rather
than by testing.

## 6. Memory the LibOS maps after a clone is invisible to that clone — the hard one

This is the deepest problem and the one still not fully solved.

An address space is created by cloning the host process, so it is a snapshot.
Any mapping the LibOS establishes *afterwards* exists only in whichever address
space was current when it was made. Every guest forked before that point faults
on it.

It is not a rare case. Caladan allocates uthread stacks and slabs of runtime
heap on demand; glibc extends its heap; anything can `mmap`. The failure is
delayed, load-dependent, and presents as a fault at an address that looks
perfectly valid in the address space you are debugging from.

Two mitigations, neither complete:

- **Pre-allocate the pools.** Reserve the stack and large-page pools up front as
  single sparse shared mappings. This is what is implemented. It bounds the
  problem to allocators you remembered, which is not the same as solving it.
- **Share the page tables.** Point every address space at the same page-table
  subtree for the LibOS's range, so a mapping made through any of them is made
  for all of them. This is the real fix, and it is hard for reasons that get
  their own section below.

**The lesson**: "make the LibOS's memory shared" and "make the LibOS's *address
space* shared" are different requirements. Shared memory only fixes mappings
that already exist at clone time.

## 7. Shortcuts that were invisible with one address space

Two were found by running the semantics tests:

- `usys_mmap()` quietly downgraded `MAP_SHARED|MAP_ANONYMOUS` to private. With
  one address space there was only ever one copy, so nothing could tell. It is
  exactly wrong once a process can fork — shared anonymous memory is *only*
  meaningful across a fork.
- `KernelMMap()` unconditionally OR'd in `MAP_PRIVATE`. Passing `MAP_SHARED`
  through it produced `MAP_SHARED|MAP_PRIVATE`, which the kernel reads as
  `MAP_SHARED_VALIDATE`, and rejected with `EINVAL`.

**The lesson**: look for places where the old invariant let someone take a
shortcut. They will not be commented as shortcuts; the first one described
itself as "silently turn shmem requests into anonymous private memory".

## 8. Making a rare operation common surfaces latent bugs

Two pre-existing Junction bugs appeared within seconds of running a fork storm:

- `ForEachProcess()` dropped a `shared_ptr<Process>` while holding
  `pid_map_lock_`. If that was the last reference, `~Process` ran and took the
  same non-recursive spinlock to deregister itself. Nothing hit it before
  because processes rarely died while the map was being walked.
- `DoWait()` wrote the exit status into the caller's memory while holding a
  spinlock. A bad pointer there faults with preemption disabled, which is
  unrecoverable, so Junction aborted the whole container — where Linux returns
  `EFAULT`. This was found by a one-line test program that called `wait(pid)`
  instead of `wait(&status)`.

**The lesson**: neither bug was in the new code. Adding `fork()` changed which
paths are hot, and the bugs were waiting there.

## 9. Identical semantics means reproducing the warts

"Identical to Linux" is a higher bar than "correct". Linux reaps a child and
*then* copies the exit status to userspace, so a bad status pointer loses the
exit status permanently and a second `wait()` returns `ECHILD`. A sensible
implementation would validate the pointer first and leave the child reapable.
That would be better behaviour and a semantic difference.

Similarly: a process killed by a signal must report `WIFSIGNALED`, not
`WIFEXITED` with `128 + signo`; interval timers must not be inherited; pending
signals must not be inherited while the signal mask is; `FD_CLOEXEC`
descriptors survive fork and are closed only by exec.

**The lesson**: write the tests against a real Linux first and let them tell you
what the behaviour is. Several of these are not what you would guess.

## 10. Interactions with the rest of the system are not optional to understand

Creating an address space creates a short-lived host task. Caladan's `ksched`
module hooks `sched_switch` and unloads a core's UINTR state whenever that core
runs a task that is not its assigned kthread. So every `fork()` silently
disarmed user interrupts on that core, and the next `SENDUIPI` raised `#UD`.

It presented as `SIGILL` inside the LibOS at an address in Junction's own text.
The diagnosis came from dumping the faulting opcode: `f3 0f c7 f0`.

**The lesson**: a mechanism that creates kernel-visible objects at runtime will
interact with everything else that watches those objects. Junction had never
created a host task after startup before.

---

## 11. Sharing page tables: why this is the hard part

The remaining problem (§6) is solved by having every address space point at the
same page-table subtree for the LibOS's range. The mechanics are easy. The
reasons it is dangerous are not.

### Reverse mapping and TLB invalidation

A guest address space has no VMA covering the LibOS; only the original does. So
when the kernel unmaps a LibOS page, reverse mapping finds only the original's
VMA. Clearing the PTE is globally visible — the table is shared — but the TLB
flush is targeted at `mm_cpumask()` of the original mm, and a core running a
guest is not in it. Even if the IPI were delivered, `flush_tlb_func()` compares
against that core's `loaded_mm`, sees a different mm, and skips the
invalidation as a lazy-TLB case. The core keeps a valid TLB entry for a page
that has been freed and reused.

Adding those cores to `mm_cpumask()` does not help, for the second reason.
Making the flush unconditional means treating the range like kernel memory,
which means intruding on the flush path.

The resolution is to make the range never *need* a flush: establishing a new
mapping requires no invalidation on x86, so a range that is only ever added to
— pinned, no THP, never unmapped, never write-protected — is safe. That is a
real constraint on the LibOS, and it has to be stated as an invariant because
nothing will enforce it.

See `docs/shared-page-tables.md` for the full analysis.

### Page-table primitives are not symmetric under runtime folding

This one took the machine down.

x86-64 with `CONFIG_PGTABLE_LEVELS=5` on a CPU without `la57` folds the p4d
level *at runtime*. Under folding:

```c
#define pgd_clear(pgd)  (pgtable_l5_enabled() ? native_pgd_clear(pgd) : 0)
```

`pgd_clear()` is a **no-op**, while `set_pgd()` writes unconditionally. Code
that installs a shared entry with `set_pgd()` and removes it with `pgd_clear()`
therefore installs it and never removes it. The cloned address space is then
torn down still pointing at the original's tables, and `exit_mmap()` walks into
them and frees the LibOS's page tables while the LibOS is running on them.

The result is immediate memory corruption with no oops written to disk — a hard
hang, nothing in the journal, reboot.

The correct primitives are the p4d ones. `native_p4d_clear()` calls
`native_set_p4d()`, which writes unconditionally and also handles the PTI
mirroring that folding requires. `p4d_offset()` resolves to the pgd entry when
folded and to the 512 GB-stride entry when not, so p4d-level code is correct on
both 4- and 5-level kernels.

**The lesson, and it is the main one**: this class of bug is not caught by
reading the code, because `set_pgd`/`pgd_clear` look like an obviously matched
pair. It is caught by running it somewhere a crash costs nothing.

## 12. Testing: a VM is not optional

Every other bug in this document cost a debugging session. The page-table one
cost a reboot of the machine, and would have cost data if anything unsaved had
been in flight.

Kernel-module work here has a failure mode that userspace work does not: the
blast radius is the machine, and the evidence is usually destroyed by the same
event that produces it. There is no stack trace to read afterwards.

The harness in `scripts/` boots the host's own kernel image under QEMU with a
minimal initramfs containing the module and `as_test`, so a bad change costs
about ninety seconds. The rule that follows is simple and should not be
negotiable:

> **No version of the module is loaded on the host until that exact build has
> passed `as_test` in the VM.**

The first version of the module was validated that way and behaved. The
page-table version was not, and did not.
