# Implementing fork() correctly: the full plan

`fork()` already works — 46/46 semantics checks matching native Linux. What is
not yet correct is **LibOS memory mapped after an address space is cloned**,
which is invisible to that address space. `docs/libos-memory-status.md` has the
evidence; `docs/libos-memory-plan.md` has the argument. This is the build order.

## The two constraints everything else follows from

**1. rmap enumerates per VMA, and `vma->vm_mm` is single-valued.** So N address
spaces need N VMAs over shared backing, not one shared VMA. That is what the
memfd is *for*: it makes N separate VMAs legal (a common object to refer to) and
free (the same physical pages). It does not avoid TLB shootdowns — it makes them
visible to the kernel's existing machinery. Verified: `docs/traces/tlb_punch.c`.

**2. `%gs` is Caladan's, `%fs` is the guest's.** Caladan's per-thread state is
`%gs`-relative (`inc/base/thread.h`), which is why LibOS code runs correctly
inside a guest syscall handler. glibc uses `%fs` — including `errno` — so any
libc call from that context reads the *guest's* TCB and dies. Measured:
`--debug_libos_alloc` segfaults in `__libc_malloc` on the arena lock.

> **Therefore: every line of arena and fault-handler code must be glibc-free.**
> Pool allocation, raw `ksyscall`, manual formatting. This is the same
> discipline `ksys_*` and `memtrace.cc` already follow, and now there is a
> stated reason for it.

## The correctness invariant

> An address range is never reused for a different `(fd, offset, prot)` binding
> until it has been unmapped from every mm that materialised it.

With it, every divergence between address spaces becomes an *absence*, absences
fault, and the fault handler repairs them lazily. Nothing eager, nothing on the
mm-switch path.

---

## Phase 0 — Guardrails

Independent of everything else, cheap, and they make the rest safe to build.

**0a. Fix the LibOS/guest discriminator.** `syscall_trap_handler` currently uses
`if (unlikely(!preempt_enabled()))`, commented "probably from junction libc".
It is wrong today: a LibOS syscall made with preemption enabled is dispatched to
the *guest* syscall table, which is how `brk` ends up as
`Unexpected syscall while in_kernel (brk)`.

Replace with the trapping RIP:

```c
bool from_libos = ctx->uc_mcontext.rip >= kVirtualAreaMax;
```

Sound because the two glibcs are distinct images either side of the partition —
Junction's at `0x7ffff78ad000`, the guest's at `0x4ffffddba000` from
`install/lib/libc.so.6` — and `usys_mmap` confines guests below. Not `fsbase`:
`usys_arch_prctl` does not range-check it, so a guest could set it above
`kVirtualAreaMax` and have its own mappings served from the shared LibOS arena.

*Verify:* `--debug_libos_alloc 128` no longer reports the `brk` misroute.

**Status: done.** `seccomp.cc` now tests
`ctx->uc_mcontext.rip >= kVirtualAreaMax`. The probe's `preempt_disable()`
scaffold — which existed only to make the old heuristic classify correctly — was
removed, so the LibOS `malloc` now runs with preemption *enabled*, the state
every guest syscall handler is in:

```
debug: guest pid 2 ... preempt_enabled=1 (must work with preemption ENABLED)
debug: LibOS malloc(128 MB) -> 0x7e9fcffdf010 [libos-range] in address space 4294967297
```

No `Unexpected syscall while in_kernel (brk)`. `scripts/fork_test.sh` still
13/13 + 46/46 + 46/46. The mapping is still `ABSENT` from the other address
space — that is the bug Phases 1-3 fix, not this one.

**0b. Assert the frozen-after-init invariant.** The init-time shared regions
(iokernel shm, SysV segments, hugetlbfs, directpath) are safe because nothing
changes them structurally after the first guest exists. That holds today by
accident. Add a check in `KernelMRemap` and `KernelMProtect` that the target is
a guest range or that init has not finished. A comparison, and it turns a future
silent bug into a loud one at the call site.

**Status: done.** `CheckFrozenViolation()` in `memtrace.cc`, called from
`KernelMUnmap`, `KernelMProtect`, `KernelMRemap` and `KernelMAdvise`. Always
on, not behind `--trace_libos_mem`: one relaxed atomic load before the first
clone, which is what the propagation-hazard check should have been and was not.

Four operations, chosen rather than assumed. `munmap`, `mprotect` and `mremap`
change a mapping other address spaces already have; `madvise(MADV_DONTNEED)` is
included because on shared memory it silently means something else and on
private memory it discards. `MADV_REMOVE` is deliberately *not* flagged: it
punches the shared object, which propagates through `i_mmap`, and is what
freeing is supposed to do.

Measured before trusting it: zero reports across 39 ctest tests, 13+46+46
fork_test checks, and the exec suite -- and it immediately caught memfs tearing
down a 256 MB extent in one address space while a forked child held a live VMA
over it. `scripts/guardrail_test.sh` tests both directions.

**0c. Extend `AuditLibOSMemory()`** from "is anything private and writable" to
"do all live address spaces agree on the LibOS range", by diffing
`/proc/thread-self/maps` per address space. Debug-mode only. This is the net
that catches whatever this plan failed to predict.

Note `/proc/thread-self`, not `/proc/self` — the latter resolves to the thread
group leader and reports the root mm regardless of which address space the
calling kthread is bound to.

**Status: done.** `AuditAddressSpaceCoherence()` in `as.cc`, behind
`--debug_as_audit`, which runs it after every fork and at the fatal-fault path
— where it turns `segfault ... addr=380000000000` into `address space
4294967297 ... first absent from 4294967297: 0x380000000000`, naming the
missing mapping instead of leaving it to be rediscovered.

Three things it needed that were not obvious:

- a registry of live address spaces. Nothing else in Junction keeps one: a
  handle lives in the `MemoryMap` that owns it. It is in `.bss`, so the startup
  sweep makes it shared — a private registry would give each address space its
  own idea of which address spaces exist;
- pre-allocated scratch. The audit reads `/proc/thread-self/maps` while bound
  to *another* address space, so a buffer `mmap`'d at audit time would be
  absent exactly where it is needed. The buffers are allocated once from
  `InitAddressSpaces()`, before the first clone, and never freed — freeing them
  would itself be a frozen violation;
- `openat`/`pread64`/`close`, not `read`. `read` is not on the seccomp
  allowlist, so a raw `read` from LibOS code traps to the SIGSYS handler and
  returns `-ENOSYS`. It works before the filter is installed, which is why this
  only broke once the audit started running after init. `msync` is blocked for
  the same reason, which ruled out using it as a presence oracle.

Comparison is by *coverage*, not exact range equality: the kernel splits and
merges VMAs freely, so the same memory can be one line in one address space and
three in another without anything being wrong. A gap is a difference; a seam is
not.

---

## Phase 1 — Fault handler, proved out on memfs

**Status: done.** `scripts/memfs_test.sh`, 13/13, each case checked against
native Linux first. `docs/bug-memfs-extent-propagation.md` is the write-up.

memfs was the one **demonstrated** failure (`docs/traces/memfs_race.c`: fork,
parent creates a file, child reads it, segfault at `0x380000000000` in
`MemInode::Read`) and the only one reachable without scale —
`echo hi > /tmp/f; ( cat /tmp/f )` was enough. It was also the easiest consumer,
because its extents are already `MAP_SHARED` from a memfd, so it could be made
strictly 1:1 and served by a trivial handler. A real bug fixed, and the riskiest
infrastructure built against the simplest case.

**1a. Its own region, above `kVirtualAreaMax`.** Extents used to start at
`0x380000000000`, inside the guest-addressable region — which is why the
propagation hazard detector counted them as guest memory and reported nothing
for five traced workloads.

The region is **reserved at startup as one `PROT_NONE` mapping**, before the
first clone. That does three things at once: proves it is free, stops anything
else being placed inside it later, and makes it exist in every address space —
so a fault in it is always a missing *extent*, never a missing region.

A compile-time base was tried first and was wrong. The kernel puts the LibOS
image and its heap wherever ASLR decides, observed at `0x575e1a723000` and
`0x58b31b64a000`, both inside the fixed region that had been chosen. The base is
now picked at runtime with `0x600000000000` preferred, and validated.

The region also had to shrink, 32 TiB → 4 TiB (`kMaxFiles` 128K → 16384),
because a contiguous 32 TiB hole above `kVirtualAreaMax` is not reliably
available: some runs had none, others produced a region ending at 127.5 TiB. The
old 128K was nominal — the address counter broke at 6144 file creations.

**1b. One allocator instead of two.** The slot bitmap is now the only allocator
and the address is derived from it, so `offset == addr - MemFsBase()`. There
used to be a second, `next_memfs_faddr`, a monotonic address counter that never
recycled: over 40 create/delete cycles of one file, 1 offset and 40 addresses. A
fault carried no information about what belonged at the address, which is
precisely why it could not be repaired.

**1c. The fault handler.** `junction/kernel/arena.h` — a registry of managed
slots, and `RepairManagedFault()`, which maps the enclosing granule at the
matching offset and lets the instruction retry. Built with a lookup from the
start even though memfs is one entry, so Phase 2 adds entries rather than
rewriting the handler. It runs in signal context: `ksys_mmap` only, no libc, no
locks, no allocation, and idempotent so two cores repairing the same granule
race harmlessly.

**What changed from the plan as written.** Two things, both discovered by
building it:

- **Freeing an extent must not unmap it.** The plan assumed deletion would
  unmap and quarantine. It should not: other address spaces materialise extents
  on demand and keep theirs regardless, so unmapping desynchronises only the
  address space doing it — and punches a hole in the reserved region that the
  next unrelated `mmap` could be handed. `MADV_REMOVE` alone is right; it
  propagates through `i_mmap` and actually returns the memory.
- **Quarantine is not needed here, and the reason is worth keeping.** The plan
  has quarantine as Phase 2d, protecting against an address being rebound to a
  different `(fd, offset, prot)`. Under strict 1:1 with a uniform granule that
  cannot happen: the binding for an address in this region is *immutable*, so a
  recycled slot recycles the address and the offset together and a stale mapping
  is stale-but-identical. Quarantine becomes necessary in Phase 2, where the
  arena's granules vary and the binding is not derivable.

  `UnmapFromAllAddressSpaces()` was written for this and then removed: with the
  1:1 layout nothing calls it, and shipping an unexercised walk over every
  address space is worse than adding it back in Phase 5, where the GC needs it
  and can test it.

## Phase 2 — The arena

**Status: done.** `scripts/arena_test.sh`, 8/8. `junction/kernel/arena.{h,cc}`:
`InitLibOSArena()`, `ArenaMap()`, `ArenaUnmap()`, `ArenaProtect()`, and the
shadow-map repair path inside `RepairManagedFault()`. The slot is at
`0x530000000000` as planned, 512 GB, sealed memfd, `PROT_NONE` reservation.
Verified by `--debug_arena_probe`: after the first fork it maps four ranges of
different shapes, changes one's protection, frees one, and reads all four back
from every other address space -- each range verified, from that address
space's own `/proc` map, to be covered only by the `PROT_NONE` reservation
before the touch and by the repaired memfd mapping after it; 4 faults repaired
per address space visited, the read-only one reinstalled as read-only, the
freed one quarantined and not reused. Three things the plan below did not anticipate, all now in:

- **The preemption-disabled fault path never tried a managed repair.**
  `signal.cc` only consulted the guest memory map there, and only under
  tracing, so a LibOS fault in *any* managed slot taken under a spinlock was
  fatal -- memfs included. That is exactly the context Caladan's allocators
  fault from, so 3a would have hit it immediately. `RepairManagedFault()` now
  runs there too; it needs no libc and its one lock is never held across a
  fault. The probe reads back with preemption disabled on purpose, to keep
  this path under test.
- **The coherence audit skips managed slots.** A range that is absent until
  touched is the lazy repair working, not a divergence; without the skip,
  `--debug_as_audit` would report every arena (and memfs) absence.
- **`ExclusiveIntervalSet` takes an allocator.** The shadow map's nodes come
  from the one pre-reserved pool the design keeps (8 MB, `MAP_SHARED`,
  populated before the first clone, sized from a bound -- see the note in
  `arena.cc`); everything else takes the default.

Deferred, deliberately: `mremap` (the two interval sets are in place for it,
nothing calls it yet), and the `MADV_DONTNEED` translation, which belongs with
the callers Phase 3 routes. `RepairManagedFault()` now also takes the faulting
access's protection, so a write to a read-only arena range is reported as the
protection fault it is rather than repaired in a loop.

One thing for anyone writing a test: `dash` uses `vfork` for a plain command
and `fork` only for `&`, subshells and pipelines. A vfork child shares its
parent's address space until it execs, so a test that needs a second address
space has to force a real fork (`sleep 0.2 & wait`, as `guardrail_test.sh`
does).

**2a. The slot.** 512 GB at `0x530000000000` (next free above Caladan's
relocated page pool at `0x510000000000` and stack pool at `0x520000000000`). One
memfd, `ftruncate` to the slot size, `MFD_EXEC` where available with a fallback
(memfs already does exactly this), then `F_SEAL_GROW | F_SEAL_SHRINK`.

Sealing matters because the kernel enforces the size at **fault** time, not at
mmap time — measured: `mmap` succeeds 4x past EOF and the access takes SIGBUS
(`docs/traces/memfd_bounds.c`). The size is a backstop, never an allocator.

**2b. Allocator and shadow map.** Both are
`ExclusiveIntervalSet<T>` (`junction/base/interval_set.h`), which already backs
`vmareas_` and `mem_areas_`; `AllocateMMRegion()` is the shape to copy. Give it
a **pool allocator over a fixed, pre-reserved region**, because:

- the fault handler consults the map, so a fault while consulting it recurses —
  the pool must be eagerly mapped into every address space at creation;
- its nodes hold pointers that must be valid in every mm, so it must live at a
  fixed address;
- fragmentation is a non-issue: `std::map` nodes are all one size, so this is a
  slab, not a general allocator.

Sizing: 102 live LibOS mappings today; at 100x headroom, 10K nodes of ~64 bytes
is ~640 KB, about 160 pages to pre-fault per address space. Sub-millisecond
against a ~142 us fork.

**2c. Point the fault handler at the real shadow map** instead of the one-entry
stand-in from Phase 1. Every lookup goes through the map — see "Growth" below
for why deriving `offset` from `addr` is not kept as a fast path.

**2d. Quarantine.** A freed range is punched but **not reused**. This is what
makes lazy propagation correct: it converts every silent divergence into an
absence.

---

### Phase 2 follow-on — make memfs a client of the arena

Once the arena exists, memfs should stop having its own allocator, its own
granule and its own rules, and become a client of it. Phase 1 deliberately gave
memfs a private, strictly 1:1 regime because that was the smallest thing that
fixed a real bug; it is not where this should end up.

**What it buys:**

- **The 256 MB granule and the 16384-file cap both disappear.** Both are
  artifacts of fixed-size extents: every file reserves a full `kMaxSizeBytes` of
  address space however small it is, and `kMaxFiles` is just
  `kMaxMemfdExtent / kMaxSizeBytes`. The arena's allocator handles arbitrary
  lengths, so a 4 KB file would cost 4 KB of VA and the limit would become total
  bytes rather than a file count. Growth would use the same copy-free `mremap`
  path as everything else.
- **memfs gets GC.** Today it never unmaps an extent -- safe, because the
  binding is immutable, but each address space keeps one 256 MB VMA per extent
  it ever touched for the life of the process. The punch reclaims the physical
  memory; nothing reclaims the VA or the VMA. Tombstones and cursors would.
- **One fault path instead of two.** Phase 2c already forces the handler to
  consult the shadow map, because quarantine and the arithmetic fast path
  cannot coexist. If memfs keeps its own regime the handler carries both, and
  the arithmetic one is exactly the shape that is unsafe wherever bindings can
  change.

And the general argument: the bug that motivated Phase 1 was memfs having its
own allocation scheme that drifted from everything else. Two regimes is how that
happens again.

**What must not be shared: the backing object.** Guests map memfs files
*directly* --

```c
// MemInode::MMap
ksys_mmap(addr, length, prot, flags, memfs_extent_fd,
          extent_offset_ * kMaxSizeBytes + off);
```

-- so that is a guest VMA over `memfs_extent_fd`. If memfs allocations came out
of the arena's memfd, a guest mapping a file would hold a descriptor-backed VMA
over the same object that holds Junction's heap, Caladan's slabs and every other
guest's file data. Junction computes the offset and length, so it is bounded
today, but any off-by-one there stops being a memfs bug and becomes a
cross-guest LibOS memory disclosure.

So share the *mechanism*, not the store: memfs keeps its own memfd and its own
slot, and loses only its allocator, its granule and its private rules. The
managed-slot registry is already per-slot with a per-slot `fd`
(`RegisterManagedSlot`), so this costs nothing structurally.

**Sequencing.** After Phase 2, as a refactor, with `scripts/memfs_test.sh`
already in place as the regression net -- which makes the arena's first real
client one that is already tested. Doing it before Phase 2 would mean building
the general mechanism speculatively against a single caller, which is how a
general mechanism ends up shaped exactly like its first caller.

## Phase 3 — Route every allocation into the arena

Two disjoint enforcement points, because they cover different code and this is
measured, not assumed.

The split is not "Junction versus Caladan". It is **which RIP window the
`syscall` instruction sits in**, because that is the only thing the seccomp
filter looks at. Two windows are allowlisted for the memory syscalls, and a
mapping made from either one never traps and therefore cannot be intercepted at
all:

| window | contents |
| --- | --- |
| `[ksys_start, ksys_end)` | Junction's `ksys_mmap` and everything reaching it through the `KernelMMap*` wrappers |
| `[base_syscall_start, base_syscall_end)` | **Caladan's own stubs** in `base/syscall.S`: `syscall_mmap`, `syscall_mprotect`, `syscall_madvise`, `syscall_mbind` |

**3a, Caladan half: done.** The two pre-reserved pools are deleted.
`runtime_mem_region` (`base/mem.h`, `base/mem.c`) is the replacement: each
region -- large pages at `0x510000000000` (256 GB) and uthread stacks at
`0x520000000000` (97 GB) -- is backed by its own sealed memfd at a **fixed
binding**, `offset = addr - base`; the *whole* index space is reserved
`PROT_NONE` (address space, not memory); a slice is mapped from the memfd on
first use and its pages are punched out on free with the mapping kept. Junction
registers both as 1:1 managed slots, so a fault in either, from any address
space, is repaired with arithmetic, exactly like memfs.

Why 1:1 and not the shadow-map arena: these allocators are index-addressed and
reuse an address after free (`lgpage_to_addr(pg)`, `free_stacks[]`), which the
quarantine regime forbids. But a binding that never changes is precisely the
condition under which arithmetic repair is safe -- `stack_pos` only ever moves
forward, and a reused page index re-maps the identical `(fd, offset)` -- so no
rebinding ever happens and the arena's shadow map is not needed. Two regimes,
but for the reason the memfs discussion gives: share the mechanism, not the
store.

What this deletes, against the costs listed under "the pools" below: the hard
cliff (the limit is now `LGPAGE_META_ENTS` and `RUNTIME_MAX_THREADS`, the same
limits the index spaces already had), the RSS high-water mark (every free
punches), the 17 GB reserved by guesswork (`PROT_NONE | MAP_NORESERVE` costs
nothing), and the one-address-space overflow. Stacks stay unguarded, as the
pool's were, for the fork-cost reason given in `stack.c`; mapping only the
usable half would give guards back at one VMA per stack.

One more thing the fault path needed, found by the stack test: a fault in a
managed region can arrive with **no uthread running** -- the scheduler itself
touching the stack of the thread it is about to switch to, in an address space
that never saw that stack. `synchronous_signal_handler()` aborted on
`!thread_self()` before any repair was tried ("Unexpected signal delivered to
Caladan code"; the test header had predicted exactly this, "no frame to report
the fault from"). The managed repair is now the first thing the raw handler
does, for every SIGSEGV, before any check that assumes a thread: it needs no
Junction thread, no libc and no bookkeeping, and a managed fault always means
the same thing whoever took it. The two later copies (Phase 1's in
`HandlePageFaultOnSyscallStack()`, Phase 2's in the preemption-disabled branch)
are gone; there is one place.

Acceptance is the two tests that were written to crash, both now passing:
`scripts/stack_overflow_test.sh` (fork, then 2048 parent stacks handed to the
child; was 2/5 crashes even after the pool was replaced, until the handler
change above) and `scripts/pipe_heap_test.sh` (17000 pipes past the old pool;
was 3/3 crashes).

**3a, Junction half: audited, nothing to edit.** Every `ksys_mmap` /
`KernelMMap*` caller was classified:

| caller | what it maps | verdict |
| --- | --- | --- |
| `mm.cc` (5), `linuxfile.cc`, `memfs.cc:199`, `jif.cc` (2) | **guest** memory, per process | per-address-space is correct; not LibOS memory |
| `zpoline.cc`, `syscall.cc`, `shim/backend/init.cc`, `perf.h` | fixed LibOS mappings at **init** | made before any clone, inherited by all |
| `memfs.cc:173` | the memfs extent | already a managed slot |
| `arena.cc`, `memprobe.cc` | ours | know where they map |

So the allowlisted window contains no LibOS mapping made after a clone, and the
escape probe's finding ("`ksys_mmap` succeeded with no SIGSYS event") is the
window working as designed for guest memory, not a gap.

- Junction: `mm.cc` (5), `memfs.cc` (3), `jif.cc` (2), `perf.h`,
  `linuxfile.cc`, `zpoline.cc`, `syscall.cc`, `shim/backend/init.cc` — plus
  `memprobe.cc` (6) and `arena.cc` (2), which are ours and already know where
  they are mapping.
- **Caladan: `mem_map_anom()` (`base/mem.c:107`) and `runtime/stack.c:203`.**
  These are the large-page pool and the stack pool -- the two allocators that
  actually overflow and crash (`docs/pools-are-load-bearing.md`). They use
  Caladan's allowlisted stubs, so no amount of SIGSYS work will catch them.
  This is the correction to an earlier version of this document, which said
  Caladan reached the kernel through libc `mmap` and would therefore be covered
  by 3b. It is covered by neither until these two call sites are changed.

**3b: done.** `RouteLibOSMemSyscall()` (`arena.cc`) sits in the trap handler's
libc branch (`seccomp.cc`), before the native fallback. Anonymous private
`mmap` comes out of the arena; `munmap`, `mprotect`, `mremap` and `madvise` of
arena memory go back to it; everything the arena declines -- file mappings
(`dlopen`), shared memory, huge pages -- still runs natively, and the memory
trace shows which. Two glibc behaviours needed their own translation, as the
table below predicted: `MADV_DONTNEED` (and `MADV_FREE`) become a hole punch
with the mapping kept (`ArenaDiscard`), and `shrink_heap`'s `MAP_FIXED
PROT_NONE` over its own heap tail becomes discard + protect rather than a
rebinding. `mremap` is implemented for all three cases -- shrink, grow in place
(offsets contiguous with the last piece when free, so the entry merges; any
free offsets otherwise), and move with `MREMAP_MAYMOVE` (the bytes never move;
the old address is quarantined without punching, since its offsets live on at
the new address -- `moved_` on the entry records that for GC).

This deletes the last pre-reserved pool: the 64 MB pre-grown heap and the
three `mallopt` calls in `InitAddressSpaces()`, which existed to stop glibc from
mapping anything after the sweep. glibc may map freely now; it all lands in the
arena.

Acceptance: `scripts/heap_overflow_test.sh`, which was written to crash. A
128 MB `malloc` forced under a forked guest's address space lands at
`0x537f...` -- inside the arena -- and from another address space is *absent
from the map, repaired on touch, and reads back the bytes the first one wrote*
(3/3). The probe's presence check had to become permission-aware first: the
arena's `PROT_NONE` reservation covers its whole slot everywhere, so "some VMA
covers it" was always true and proved nothing.

The original 3b text, for what it covered:

**3b. Only the libc-routed callers are intercepted**, at the SIGSYS handler:

- Junction's glibc: malloc arena growth, `dlopen`, pthread stacks. `brk` is
  already forced to return 0 so the main malloc arena cannot grow that way.
- Caladan's *libc* `mmap` callers, which are a different set from its stub
  callers: `mem_map_file` (`base/mem.c:158`), the iokernel shm mapping
  (`runtime/ioqueues.c:151`), and directpath
  (`mlx5_init_external.c:118` and `:192`).

The handler already sees these — today it re-executes the call natively and
returns the result, which is why the trace comment there reads "landing in
whichever address space this core is currently bound to". The hook exists; the
redirection is what Phase 3 adds.

**A note on the pools.** They are `MAP_SHARED` *anonymous*, not memfd-backed,
which is why they have a hard cliff instead of a repairable fault: with no file
behind them there is no `(fd, offset)` for a fault handler to reconstruct.
Folding them into the arena is what turns that cliff into a fault, and it is
the main reason 3a matters more than its position in this list suggests.

All three operations must be handled, not just `mmap`:

| trapped | arena implementation |
| --- | --- |
| `mmap` | allocate a VA range, map `offset = addr - base`, record it |
| `mremap`, same size | **move the VMA to a fresh VA range, same offset.** Update the shadow entry, quarantine the old VA. No copy, no punch — the bytes never move. |
| `mremap`, growing | extend the offset range in place if free, else stay VA-contiguous and fragment the offsets into several VMAs; see below |
| `munmap` | punch the hole, quarantine the VA and offset ranges together |
| `madvise(MADV_DONTNEED)` | translate to `MADV_REMOVE`; see below |

`mremap` is not optional: glibc relocates large chunks on `realloc`
(`docs/traces/realloc_probe.c` — `MREMAP_MAYMOVE`, and it moves). Intercepting
`mmap` alone means the first large `realloc` breaks the invariant silently.

**Why a plain move needs no copy.** The content lives at a memfd offset and
stays there; only the virtual address changes. After mm A moves `[X, X+L)` to
`[Y, Y+L)`:

- mm B follows a fresh pointer to `Y` → nothing mapped → faults → the shadow map
  says `Y -> (fd, K)` → mapped correctly;
- mm B follows a stale pointer to `X` → still mapped to offset `K` → reads the
  right bytes. More permissive than a single mm (which would fault), never
  wrong;
- nothing is ever placed at `X` again, because the allocator only hands out VA
  ranges no mm has mapped. That is what quarantine buys.

`docs/traces/arena_demo.c` shows the failure this avoids — but note it
demonstrates the *strict 1:1, arithmetic-only* regime, where there is nowhere to
record an exception and the copy is therefore forced. With a shadow map there
is somewhere to record it, and the copy is not needed. The `copy` mode of that
demo is the 1:1 fix, not the design here.

**Growth: two allocators, no copy.** Keep *two* interval sets — one over the
LibOS virtual address space, one over the memfd's offset space — instead of
deriving one from the other. Then `mremap` with a larger size either finds the
offsets past `K` free and extends in place, or keeps the range **contiguous in
VA** while fragmenting it in the memfd. The latter is several VMAs over one
logical buffer, all with the same protections, described by several shadow
entries. Both spaces are enormous, so the fragmenting case should be rare, and
neither case copies.

This drops the arithmetic fast path — `offset` can no longer be derived from
`addr` in general, so the fault handler always consults the shadow map. That is
a simplification rather than a cost: one rule with no special cases, and a
`std::map` lookup (~100 ns) on a path that is already a page fault.

Quarantine then covers the **pair**: a VA range and its offset range are
released together, once GC has unmapped the VA from every mm. Keeping them
paired costs nothing and avoids reasoning about a stale VMA whose offset has
been recycled underneath it.

### Freeing means punching, and `MADV_DONTNEED` has to be translated

Measured on a 64 MB memfd (`docs/traces/memfd_reclaim.c`):

```
after writing 64 MB                  resident in memfd:    64 MB
after MADV_DONTNEED                  resident in memfd:    64 MB   first byte 0x78
after munmap                         resident in memfd:    64 MB
after FALLOC_FL_PUNCH_HOLE           resident in memfd:     0 MB
remapped after punch                 first byte 0x00
```

Two rules follow.

**Unmapping is not freeing.** Neither `munmap` nor `MADV_DONTNEED` returns a
single page; only `PUNCH_HOLE` does. Every path that means "give this memory
back" has to punch, or the arena grows monotonically and holds shmem pages
resident forever. (There is 8 GB of swap here, so they are evictable in
principle — but paging them is not the same as freeing them.)

**`MADV_DONTNEED` silently changes meaning on the arena, and must be
translated.** On private anonymous memory it discards, and subsequent reads
return zeros. On a shared memfd mapping it drops only the caller's PTEs: the
page survives and reads back `0x78` above. glibc uses `MADV_DONTNEED` to trim
its heap, and large `calloc` relies on freshly obtained memory being zero — so a
recycled arena range still holding old bytes would hand `calloc` dirty memory.

The fix is exact: on arena ranges, translate `MADV_DONTNEED` to `MADV_REMOVE`.
The measurement shows that restores precisely the semantics glibc expects —
memory actually freed *and* subsequent reads zero — and fixes reclamation at the
same time. For the same reason, **free must always punch**: it is what
guarantees a recycled range reads as zeros.

This has not bitten yet only because `InitAddressSpaces()` already sets
`M_TRIM_THRESHOLD` to `INT_MAX`, disabling glibc trimming. That is a landmine,
not a defence.

---

## Phase 4 — Fold the startup sweep into the arena

**Status: skipped, deliberately, except the comment.** The sweep's mappings all
predate the first clone, so every address space inherits them; they never
fault as absent, so recording them in the shadow map would never be consulted,
and consolidating 27 memfds into one is tidiness with no behaviour behind it.
The clone stack now carries the comment saying why it is exempt. Revisit only
if a mapping made *before* the sweep ever needs repairing, which today none
does.

**memfs as an arena client (the Phase 2 follow-on): deferred.** It is a real
rewrite with semantic edges the 1:1 layout does not have -- a guest that
`mmap`s a file and then *extends* it needs offsets that do not exist yet;
guest mappings over fragmented offsets must be installed piece by piece; the
snapshot archive format changes -- for a benefit that is address-space economy
(a 4 KB file costs 4 KB, the 16384-file cap goes). Today's memfs is correct,
simple, tested 13/13, and needs no GC because a slot's binding never changes.
Do it when the cap or the granule actually bites.

## Measurements after Phases 2, 3 and 5

**fork() to the child's first instruction**, medians, same harness as
`docs/multi-address-space.md`:

| parent footprint | Linux | Junction, before | Junction, now |
| --- | --- | --- | --- |
| 0 MB | 66 us | 142 us | 151 us |
| 16 MB | 216 us | 236 us | 274 us |
| 64 MB | 647 us | 531 us | 540 us |

A modest increase, largest at 16 MB, plausibly the three reservations, the
memfd-backed region VMAs, and the clone-time GC slot; single-sample medians,
so within a few runs' noise. Recorded, not chased.

**RSS does not come back, and the pools were never why.**
`docs/pools-are-load-bearing.md` listed "RSS becomes a high-water mark" as a
cost of the pools -- 190.3 MB held after 3000 pipes were closed -- because the
pooled `lgpage_destroy` skipped the `munmap` upstream did. With the pools
replaced by regions that punch on free, the same experiment gives the same
number: the large-page region holds 194,832 KB before *and after* the pipes
close (`smaps`, two VMAs: the merged memfd mapping and the reservation). The
punch never runs because nothing above the region frees a whole unit: the slab
returns a page only when every object in it is free (`slab.c`,
`item_count == nr_elems`), which tcache magazines and fragmentation prevent;
and across 1023 thread create/join cycles `stack_reclaim()` ran **zero** times
(`strace`: no `MADV_REMOVE`; the 853 `MADV_DONTNEED`s are glibc trimming
*guest* stacks), because freed stacks sit in cached magazines. So the regions
restore the *ability* to return memory -- a freed page or a reclaimed stack
is punched -- but the layers above them do not free, and upstream Caladan has
the same layers. Making the slab release empty pages, or reclaiming stacks on
thread exit, is allocator policy work, separate from address spaces, to take
on only if the high-water mark matters for a real workload.

(Incidental: a guest could create 1023 threads, then `pthread_create` failed
-- a per-process limit in Junction, unrelated to memory. Noted, not pursued.)

`ShareLibOSMemory()` already does the hard part: it reads `/proc/self/maps`
before the first guest and converts private-writable LibOS mappings to shared,
with `AuditLibOSMemory()` checking the result. Two changes:

- consolidate onto the **one** arena memfd (it currently creates 27, one per
  mapping);
- record each range into the shadow map as it goes, so the map is seeded by the
  same pass that establishes the invariant.

Keep the two-phase split. A sweep is more trustworthy than intercepting
Caladan's startup, because it catches mappings nobody remembered — that is what
the audit is for.

The clone stack (`InitAddressSpaces`, 64 KB, `MAP_PRIVATE`) should either move
into the arena or carry a comment saying why it is exempt; it is currently
skipped by the audit silently.

---

## Phase 5 — GC by epoch, applied where it is cheap

**Status: done.** `arena.cc`, "Garbage collection". The shape below, with these
specifics:

- **The tombstone log** is a 65536-entry ring under the arena lock, one entry
  per *piece* of a freed range (so release knows each piece's offsets) with a
  `free_offsets` flag that is false for a range `mremap` moved away, since its
  offsets live on at the new address. A full log leaks the range into
  quarantine for good, counted, rather than reusing it early.
- **One cursor per address space**, in a slot table in the arena; the root has
  slot 0. A `MemoryMap` caches a pointer to its address space's slot, so the
  per-syscall check in `sys_dispatch()` is two loads, `*cursor < head`.
- **Apply at syscall entry**, one batch of 64 per entry, in the address space
  that needs it while it is loaded. No `on_sched` change was needed: a thread
  that never enters the kernel is a straggler, and the collector covers it.
- **Applying means the reservation back over the range**, `MAP_FIXED
  PROT_NONE`, not `munmap` -- the same single syscall, and it keeps the slot
  whole, which is why the reservation is kept at all. This supersedes point 4
  below.
- **The collector** is a runtime thread, every 50 ms: release everything below
  `min(cursor)` over live slots (erase the dead entries; free the offsets if
  they are this range's), and under pressure -- half the log, or 256 MB
  quarantined -- visit each straggler with preemption disabled, apply on its
  behalf, and release again. `ArenaGcCollect(force)` runs one collection on
  demand for tests.
- **A clone's slot is created inside `CloneCurrentAddressSpace()`**, seeded
  with the cloner's cursor read *before* the clone. Both halves matter and the
  probe found the first: the address space exists from the clone, and a
  collection in the window before its `MemoryMap` is built must count it, or
  it is left out of `min(cursor)` while holding stale mappings. Reading the
  seed before the clone keeps the child at or behind the truth.

The property that makes the whole thing forgiving, and worth stating once:
**over-applying is harmless.** Putting the reservation over a range that is
live, or twice, only creates an absence, and the fault handler repairs
absences. So a cursor need only ever be *at most* the truth -- a new
`MemoryMap` with no better information starts at 0 and re-applies the retained
log once. Exactness is only needed in one direction.

Verified by `--debug_arena_probe` (`scripts/arena_test.sh`, 11/11): a range
every other address space has materialised is freed and collected, and is then
not readable there (checked from that address space's own map), nothing is
left in quarantine, and the entries are gone; then 256 map/free cycles of 1 MB
leave nothing quarantined, 256 MB released, and the node count back at its
baseline -- the address space was reused.

The original design, for the reasoning:

A range cannot be reused until every address space that materialised it has
dropped its mapping. Nothing tells those address spaces on its own: a VMA in
another mm is an independent kernel object, and `rmap` reaches it only for
operations on the *backing object*, never because some other mm rearranged its
own layout. So the removal has to be driven by us.

The obvious way is to walk: bind a kthread to each mm in turn, unmap, bind back.
That was the original plan here, and it is the worst part of it -- an ioctl per
mm per reclaim, on a path that has to run with preemption disabled, in address
spaces where the running thread's own stack does not exist.

**Do it lazily instead, with a tombstone log and a per-mm cursor.**

- freeing a range appends a **tombstone** `(addr, len, index)` to a global log;
- each mm carries a **cursor**: the index up to which it has applied them;
- when an mm is next loaded, it applies the tombstones past its cursor, up to a
  batch limit, and advances;
- a range leaves quarantine when `min(cursor)` over all live mms passes its
  index.

This is the same shape as Linux's lazy TLB invalidation, which reconciles
`mm->context.tlb_gen` against the per-CPU `cpu_tlbstate.ctxs[].tlb_gen` in
`switch_mm` rather than chasing every CPU at invalidation time.

Three things it buys:

- **the work happens in the mm that needs it, while that mm is loaded.** No
  binding a kthread into somebody else's address space, no preemption
  gymnastics, no ioctl per mm. The cross-address-space walk disappears from the
  common path entirely;
- **the quarantine release condition becomes exact and cheap.** `min(cursor)`
  over live mms is epoch reclamation. It replaces both the "visit the sharers"
  walk and the per-VMA membership bitmap that was going to optimize it -- the
  cursor already encodes which mms are behind;
- **the cost is bounded by the batch size**, and is paid at a switch that
  already costs an ioctl.

### Four things it needs to be correct

**1. Mark in `on_sched`, apply somewhere else.** `on_sched` runs with preemption
disabled, on the runtime stack, after `set_fsbase()` has already repointed `%fs`
at the guest, one instruction from `__jmp_thread` (`lib/caladan/runtime/sched.c`).
Issuing blocking `mmap`/`munmap` there -- with the TLB shootdowns they carry --
is the "fault with preemption disabled" class of bug. `on_sched` should do
nothing but compare the cursor and set a flag; the batch is applied at the next
safe point, syscall entry being the natural one. Nothing is lost by deferring:
the work has to happen before the range is *reused*, and the cursor gate
already guarantees that.

**2. Stragglers pin the epoch.** `min(cursor)` is held back by any mm that is
not running, so a process blocked in a long `read()` stalls reclamation for
everyone and the log grows without bound. It cannot be skipped: when it wakes it
still holds the stale VMA. So the walk survives -- demoted from the common path
to a **pressure-triggered fallback** that force-visits stragglers when the log
or the quarantine crosses a threshold. Log length is the pressure signal.

**3. The fault handler must stop being arithmetic-only.** Phase 1's handler
repairs any address in a slot by computing `offset = addr - base`. Once ranges
can be quarantined, a thread touching a tombstoned range would re-materialise
the mapping the GC is retiring -- and after reuse, with the wrong binding. The
handler has to consult the shadow map and refuse ranges that are not live. That
is Phase 2c, and this is what makes it load-bearing rather than tidying:
quarantine and the arithmetic fast path cannot coexist.

**4. Apply means `munmap`.** Decided deliberately, so here is the tradeoff.

`munmap` leaves a hole in the slot's `PROT_NONE` reservation, and the kernel's
free-space search has never heard of our quarantine. In principle it could hand
that address to the next `mmap(NULL, ...)` from Junction's glibc or Caladan, and
then two address spaces hold different objects at the same address with neither
faulting. The alternative is to re-reserve instead:

```c
mmap(addr, len, PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
```

which keeps the invariant that every address in the slot is, in every mm,
either an extent or the placeholder -- never absent.

Measured, the risk does not currently materialise: the kernel allocates
top-down from `mmap_base` (~128 TiB) and finds ~123 TiB of free space above a
slot at `0x600000000000` long before it would descend into it. An unrelated
`mmap(NULL)` issued against a deliberate hole landed outside it. So the
reservation is defence-in-depth here, not load-bearing, and `munmap` is the
simpler thing.

Two notes for whoever revisits this:

- the **tripwire** already exists. `AuditAddressSpaceCoherence()` diffs every
  live address space's mappings, so a foreign mapping inside the slot shows up
  as "absent here". That check works across address spaces, which is exactly
  what `MAP_FIXED_NOREPLACE` cannot do -- `NOREPLACE` only tests the mm issuing
  the call, and the collision can be established in a different one;
- the **upgrade is cheap but not free** if it ever does bite. Re-reserving is
  not an extra syscall -- `MAP_FIXED` subsumes the unmap -- but it is more work
  inside the same call, because the kernel installs and merges a replacement
  VMA. Measured over 2000 iterations on a 16 MB extent:

  | | `munmap` | re-reserve |
  | --- | --- | --- |
  | untouched extent | 0.59 us | 1.15 us (+97%) |
  | pages faulted in | 501 us | 502 us (noise) |

  So it roughly doubles the cost of retiring a range that has no pages, and
  disappears entirely once tearing down PTEs dominates. It also keeps the slot
  at one VMA: measured 3 -> 1 on re-reserve, against 2 and a hole for `munmap`.

  Note separately that `MAP_FIXED` cannot be the loud check -- it silently
  replaces whatever is at the address and never refuses. Only
  `MAP_FIXED_NOREPLACE` refuses, and only for the mm issuing it.

### Freeing pages is a separate job from retiring VMAs

Keep these apart; conflating them is a mistake this document made:

| | what it does | who sees it |
| --- | --- | --- |
| `MADV_REMOVE` on the range | frees the object's pages | every mapper, automatically, via `i_mmap` |
| tombstone + cursor | retires the stale VMAs | each mm, when it next runs |

`MADV_REMOVE` does **not** unmap anything and does not zero anything eagerly. It
punches the backing object: the folios are dropped from the page cache, and
every PTE pointing at them is zapped through `unmap_mapping_range()`, which
walks the object's `i_mmap` tree across every mm. The zeros appear later and
lazily, when someone faults a hole in the sparse object and the kernel allocates
a fresh zeroed page -- exactly like an anonymous fault. Verified across address
spaces in `docs/traces/tlb_punch.c`: a child in a forked mm observes `0x00`
after the parent punches.

So the punch is what returns memory, and the GC is what removes the VMAs.
Neither substitutes for the other. The protocol that follows:

| who | when | does |
| --- | --- | --- |
| the freer | at free | `fallocate(PUNCH_HOLE)`, `munmap` its own VA, append the tombstone |
| every other mm | at its cursor | `munmap` its VA. No punch: the pages are already gone |
| the allocator | when `min(cursor)` passes the index | release the range from quarantine |

There is no race over who punches. Freeing is one event in the shadow
allocator, serialized by its lock; other address spaces never decide to free
anything, they only apply tombstones. Exactly one punch, by construction.

Punch with `fallocate(fd, FALLOC_FL_PUNCH_HOLE, off, len)` rather than
`MADV_REMOVE`. `madvise` needs the range still mapped in the caller, which
couples the punch to the freer's own unmap ordering; `fallocate` acts on the
object whether or not anyone has it mapped. (memfs can keep `MADV_REMOVE` --
it never unmaps -- but the arena should not inherit the constraint.)

Note also that the punch's reclaim is not final until the last VMA is retired.
A straggler that touches the range before applying its tombstone faults a fresh
zero page back into the object, and the kernel serves that from the straggler's
own VMA without Junction seeing it -- so the fault handler cannot refuse it.
That is a use-after-free by the LibOS and out of contract, but it does mean the
GC, not the punch, is what ultimately bounds memory.

Two measurements still say the whole thing is close to free: **LibOS allocation
events after init are 0** across all five traced workloads, and `max_map_count`
here is 16777216 against 102 live mappings.

## Real applications (`scripts/app_test.sh`)

Everything above tested the mechanism with a program written to reach one
path. This runs ordinary software that forks the way software does -- shells
spawning pipelines and subshells, Python's `multiprocessing` (fork start
method) and `subprocess`, `make -j`, `git` -- natively and under Junction, and
requires the output to match. A subset also runs under `--debug_as_audit`.

**Passing:** the `sh` and `bash` loops (240 backgrounded subshell pipelines,
16 in flight), `multiprocessing.Pool(8)` five times over 2000 tasks (output
identical to native), `subprocess.run` x150, `git log`, and 30 s of worker
pools created and torn down (9/9 standalone runs) -- with the audit finding no
divergence on any of them.

**What it found, in order of what it means:**

1. **A real MAS-exposed bug, fixed.** `poll()`'s per-fd trigger wrote `revents`
   straight into the poller's guest `pollfd` array -- on the *waker's* thread,
   which under MAS may be another process bound to another address space where
   that memory does not exist (`MADV_DONTFORK`). Deterministic crash in
   `subprocess.run`. Now the trigger records into a LibOS-side buffer and the
   poller copies back in its own address space, as `select` already did. The
   rule: LibOS code touches a guest's memory only on that guest's own threads;
   wakers communicate through LibOS memory. Every other callback site was
   checked; this was the only one. Switching the waker's mm would have been the
   wrong fix -- an ioctl per wakeup, and a thread running in a foreign address
   space with preemption disabled.
2. **An audit false positive under concurrency, fixed.** The coherence audit
   classified "LibOS mapping" by the current process's regions, so another live
   guest's memory -- correctly absent from every clone but its own -- was
   reported as a failed propagation. It classifies by the partition now.
3. **A pre-existing syscall-number collision, root-caused, not yet fixed.**
   `cp -r` dies deterministically. Junction uses syscall-table slots 452-455 for
   its own entry trampolines (`systbl.py`, and `JUNCTION_ENTRY_*` in the guest
   glibc's `sysdep.h`), chosen in mid-2023 when Linux stopped at 451. Linux 6.6
   made 452 `fchmodat2`, and coreutils 9.4's `cp -r` calls it once per
   directory. The call dispatches into the entry trampoline as if it were a
   handler, which re-reads a stale value as the syscall number and transforms it
   again -- the fault at `0x1207100` is `(0x200E20 << 3) + 0x200000`, and
   `0x200E20` is `&table[452]`. Nothing to do with address spaces; it blocks
   `make` only because the test copied its inputs with `cp -r`. The fix is to
   move the entry slots out of the Linux number range on both sides -- Junction's
   `systbl.py`/`syscall.cc` and the guest glibc's `sysdep.h` -- and give 452 a
   real `fchmodat2`. The glibc half is in `lib/glibc`, which has uncommitted
   work of its own, so it is not done here without a decision.
4. **A pre-existing `wait4` semantics bug, fixed.** With `WNOHANG` and children
   that exist but are not yet ready, `DoWait` returned `EAGAIN`; Linux returns
   0 (`ECHILD` only when there are no children at all). GNU `make -jN` polls
   with `WNOHANG` and treats `EAGAIN` as fatal ("wait: Resource temporarily
   unavailable"), so every parallel make died and `-j1` -- a blocking wait --
   passed, which is exactly the threshold measured. One line, unambiguous
   semantics, in the wait path this work lives in.
5. **A pre-existing `pselect6` ABI bug, fixed.** The syscall's sixth argument
   is a pointer to `{const sigset_t *ss; size_t ss_len;}` -- Linux packs it
   because `pselect6` has run out of registers -- and glibc builds exactly
   that. Junction read a `sigset_t` there, so the *address* of the mask was
   used as the mask: which signals were blocked during the wait depended on
   the stack layout. GNU make's jobserver wait is `pselect` with SIGCHLD
   unblocked; its argpack landed on an address with bit 16 set, so SIGCHLD was
   blocked for the wait, and `make -jN` slept forever after its last child
   exited (visible only once the `WNOHANG` bug above stopped killing it
   first). Found by the trace: the decoded mask contained `SIGKILL` and
   `SIGSTOP`, which nothing can block. `ppoll` passes its set directly and
   was never affected.
6. **A pre-existing restart-semantics bug, fixed -- and the one that actually
   hung `make -jN`.** Once the two bugs above were out of the way, `make -j2`
   still slept forever, and every hypothesis about *why* the wakeup was lost
   (zombie-before-SIGCHLD ordering, the `pselect` mask swap, a stale
   `notified_`, inherited pending signals) was tested and refuted -- the
   instrumented run showed every SIGCHLD to `make` was enqueued, passed the
   thread check, and sent its IPI. The trace then showed the real sequence:
   `pselect6` → SIGCHLD handler runs → `rt_sigreturn` → `pselect6` again with
   identical arguments, and no `wait4` in between. Junction **restarted the
   syscall after the handler** because the handler had `SA_RESTART` (glibc's
   `signal()` default). Linux never restarts `select`/`pselect`/`poll`/`ppoll`/
   `epoll_wait` once a handler has run (`signal(7)`), and `make`'s loop is
   exactly "pselect until SIGCHLD, then reap". `DoPoll`, `DoSelect` and epoll's
   `Wait` returned `-ERESTARTSYS`; they now return `-ERESTARTNOHAND`, which the
   existing restart logic turns into `EINTR` when a handler ran and a
   transparent restart when none did -- Linux's rule.
7. **Two bugs in this branch's own address-space bookkeeping, fixed -- and the
   second is the one behind `make -j6` and up.** With the restart bug gone,
   `-j2/-j4` passed and `-j6/-j8` died with `mm: Create wanted 0x4fd... got 0`
   / `MM Panic`: an exec's fresh guest slice, free in the global registry, was
   already mapped in the exec'ing process's address space. Two theories were
   tried and refuted by the test before the mechanism was *measured*, with a
   log of every slice map/unmap, every clone and every exec, plus a dump of
   what was really mapped at the colliding range:

   - **The collision.** The fork exclusion list (`MADV_DONTFORK`) was built by
     walking *live processes*. A process that has exited leaves that walk
     before its `MemoryMap` destructor has finished unmapping its image. The
     trace: `make` reaps pid 5 at 0.042989 (teardown begins); pid 12 forks at
     0.042999 -- pid 5 is no longer a process, so its slice is not excluded --
     and the clone copies pid 5's still-mapped image; the slice is then freed
     and handed to pid 19's exec in that clone, `EEXIST`. The list is now built
     from the region **registry**, which holds a range from allocation until
     *after* its unmap completes, minus the forker's own range and minus the
     LibOS's fixed pages (syscall table, vDSO -- registered separately now, so
     they are never excluded). Adjacent slices merge in the registry, so the
     cut is done per interval rather than by comparison.
   - **Ownership on vfork+exec.** `exec` called `TransferAddressSpaceTo`
     unconditionally, including for a vfork child whose `old_mm` is its
     *parent's*, so in a forked address space a vfork+exec'd pipeline member
     came to *own* the space: on exit it released it under the still-running
     shell and skipped unmapping its image. The owner bit is replaced by a
     **reference count** shared by every map living in a forked space -- the
     forker's and each vfork+exec'd child's -- released by the last holder,
     while every other map removes only its own image. Real, and it also let
     a vfork child's exit forget the parent's arena GC cursor slot; but it was
     not the collision, which is why fixing it alone still failed 6/6.

   Neither is pre-existing: both are in the address-space implementation this
   branch added, reachable only by fork-and-exec-heavy programs at
   concurrency. `make -j6`/`-j8` now pass 6/6 with native's answer.
8. **Pre-existing limits, pinned:** one non-relocatable binary at a time
   (`cc1` is one, so `make -j` cannot drive gcc; per-process address spaces are
   what could lift this); and a per-process thread limit a guest hits at 1023.

Large pages are hugetlb-backed again (`739cdfc`): the region had quietly
changed them to 4 KB shmem pages, found because hugetlb pages are invisible to
RSS -- which is also why the "RSS high-water mark" was never the pools' doing.

## What needs no work

- **directpath.** Two mappings, both `MAP_SHARED`, both in `mlx5_init_ext_late`,
  reached once from `runtime_init` before the sweep. The UAR doorbell is per
  *kthread*, so inheritance by `clone()` is required, not a hazard — and it is a
  file mapping of the VFIO fd, so `unmap_mapping_range()` walks `i_mmap` and
  finds every address space, exactly as with the memfd.
- **iokernel shm, SysV segments, hugetlbfs pools.** Already `MAP_SHARED` and
  established before the first guest, so `clone()` inherits them coherently.
- ~~**Caladan's two runtime allocators** (`stack_create`, `lgpage_create`).
  Pre-reserved and hooked; never overflowed across five workloads including 512
  concurrent guests.~~ **Wrong, and measured wrong later.** Both overflow, and
  both crash deterministically when they do: 8000 threads past the stack pool
  (5/5), 17000 pipes past the large-page pool (3/3, pure guest code). The pools
  are a *mitigation*, not infrastructure that needs no work — see
  `docs/pools-are-load-bearing.md` for the two-branch comparison, and note what
  they cost: RSS is a high-water mark that never returns (190.3 MB held after
  3000 pipes were closed), `stack_reclaim()` is never called, 17 GB of address
  space is reserved by guesswork, and there is a hard cliff at each edge.

  This reframes the whole plan. The pools are not something the fix is built on
  top of; they are what the fix **deletes**.
- **Page-table / maple-tree sharing.** Rejected on constraint 1. The kernel
  module stays switch-only, and `enable_pgtable_sharing` stays off.

## Verification gates

- `scripts/vm_test.sh` **must pass for the exact build before any `insmod`** —
  a page-table bug took the host down once already.
- `scripts/fork_test.sh` — 13/13 module, 46/46 native, 46/46 Junction.
- `docs/traces/memfs_race.c` — must match native.
- `--debug_libos_escape` — all shapes inside the slot.
- `--debug_libos_alloc` — allocation visible from every address space.
- The five-workload trace set, plus `scripts/directpath_trace.sh` when the NIC
  can be reconfigured.
