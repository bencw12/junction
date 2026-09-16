# Plan: making LibOS memory correct under multiple address spaces

Working plan, kept here so it can be picked up later. Background is in
`docs/multi-address-space.md` (what was built), `docs/shared-page-tables.md`
(the TLB analysis), and `docs/fork-challenges.md` (what went wrong and why).

## The problem, stated once

A guest address space is a clone of the host process, so it is a snapshot.
Anything the LibOS maps *afterwards* exists only in the address space that was
current when it was mapped. Guests forked earlier fault on it; guests forked
later inherit it. The failure is delayed, load-dependent, and looks like a fault
at an address that is perfectly valid in whichever address space you are
debugging from.

Pre-allocating Caladan's stack and large-page pools bounds this to allocators we
remembered. It does not close the class.

## Why the obvious fixes were rejected

**Shared page tables** (implemented, then reverted). Point every address space
at the same page-table subtree for the LibOS range. Works, and is VM-verified,
but:

- Guest address spaces have no VMA over the range, so reverse mapping only finds
  the original mm. A TLB shootdown then targets the wrong `mm_cpumask`, and
  `flush_tlb_func()` discards it on any core running a different mm. The stale
  entry persists under *that core's PCID*, because the guest mm's `tlb_gen` was
  never bumped. Use-after-free.
- Safe only if the range never needs a flush at all — pinned, pre-populated,
  never unmapped or write-protected. An invariant nothing enforces.
- One uniform window VMA would also destroy W^X: the fault path consults
  `vma->vm_flags`, not the PTE, so a writable window makes LibOS text writable
  the moment anything faults on it.

**Sharing the maple tree nodes.** Elegant, but `vma->vm_mm` is single-valued.
rmap enumerates *per VMA* (`rmap_one(folio, vma, ...)`), so N mms sharing one
VMA gives one flush target — trading the VMA problem straight back for the TLB
problem. The maple tree also has no subtree-ownership concept and rebalances
across any boundary, unlike page tables where hardware fixes the levels.

## The design being pursued

**Per-mm VMAs over a memfd-backed LibOS arena, propagated lazily.**

The LibOS's address range is backed by one sparse memfd, with
`offset = addr - slot_base` — the memfd *is* the LibOS address range, as a file.
Propagating a mapping into another address space is then one line and needs no
kernel support:

```c
mmap(addr, len, prot, MAP_SHARED | MAP_FIXED, libos_memfd, addr - SLOT_BASE);
```

Verified properties (see `scratchpad/memfd_probe.c`):

- a 512 GB sparse memfd is fine
- two mappings of the same offset are the same physical pages
- protections are per-mapping — an RX and an RW mapping of one memfd coexist,
  so W^X survives
- `fallocate(PUNCH_HOLE)` zaps *every* mapping of the range, everywhere, with
  the shootdowns, via the kernel's `i_mmap` walk — unmap propagation is free

Because each address space gets its own VMAs and its own PTEs over shared shmem
pages, this is an ordinary shared file mapping: rmap, reclaim, migration and TLB
invalidation are all native. No page-table surgery, and the kernel module goes
back to doing only the mm switch.

Cost is a fault per address space per page touched, one-time — a few hundred
faults per guest, well under a millisecond.

### Why the arena needs an interception to work at all

Junction's seccomp filter allows `mmap` only from the `ksys_start..ksys_end` RIP
range, so glibc's `mmap` traps. The SIGSYS handler kills `brk` outright
(`rax = 0`) to force Junction's glibc onto mmap, and then executes the trapped
mmap natively via `ksys_default`. That creates a private anonymous mapping **in
whichever address space the calling core is bound to** — normally a guest's. So
LibOS heap memory can be owned by a guest and freed when that guest exits.

`mallopt(M_MMAP_THRESHOLD, ...)` does not prevent it: glibc clamps the
threshold, and with `brk` dead the main arena cannot grow, so everything past
the pre-grown cushion goes to mmap.

The fix lands at that same choke point, which exists by construction:

```c
if (sysn == __NR_mmap && (flags & MAP_ANONYMOUS) && !(flags & MAP_FIXED)) {
    ctx->uc_mcontext.rax = LibOSArenaMMap(len, prot, flags);
    return;
}
```

This is also the real answer to "how do we guarantee nothing maps outside the
arena": the seccomp filter already denies it. glibc cannot obtain memory another
way.

### Telling Junction's glibc from a guest's

Today the discriminator is `if (unlikely(!preempt_enabled()))`, commented
"call probably from junction libc". It is a heuristic. LibOS code that runs with
preemption enabled will have its glibc `mmap` dispatched to `usys_mmap` against
the current guest's memory map.

Sounder discriminators, in increasing order of cost:

- **Address range.** Guests are confined below `kVirtualAreaMax`
  (`0x500000000000`); the LibOS lives above it, plus the low trampoline pages.
  A single comparison on the trapping RIP classifies reliably. This is a
  property the slot partitioning already established.
- **Thread state.** `!IsJunctionThread()` (a Caladan-internal uthread) or
  `mythread().in_kernel()` (already inside Junction) means the syscall is
  LibOS-internal, semantically rather than incidentally.

## Order of work

1. **Revert the page-table sharing** from the module, back to the VM-verified
   switch-only version. Done before anything else is added.
2. **Instrument and measure.** Trace every LibOS memory operation while running
   real applications: anonymous map/unmap, file map/unmap, protection changes.
   Record enough context to attribute each to the LibOS or a guest, and to see
   how often the `!preempt_enabled()` heuristic disagrees with a sound
   classifier. The point is to understand what applications actually do, not to
   decide whether a case is rare enough to skip.
3. **Memfd arena + redirect** at the SIGSYS choke point.
4. **Lazy propagation**: the fault path first (it is the correctness backstop),
   then a generation counter checked in `on_sched()` as the optimisation.

## Measuring it

`--trace_libos_mem <path>` records every memory mapping operation the process
performs, and `scripts/memtrace_report.sh` summarises one.

```
build/junction/junction_run build/junction/caladan_test.config \
    --trace_libos_mem /tmp/t.txt -- /usr/bin/python3 work.py
scripts/memtrace_report.sh /tmp/t.txt
```

Each record carries who asked (Junction deliberately, Junction's glibc via the
seccomp trap, the Caladan runtime, or a guest syscall), where the address landed
(guest range or LibOS range), the protections and flags, the calling return
address, and the Junction context at the time -- guest pid, thread, bound
address space, `in_kernel`, and preemption state.

The attribution is decided at each call site rather than inferred, because the
production discriminator is not sound. Recording the preemption state alongside
a known-correct classification also measures how often
`!preempt_enabled()` would have misfiled a LibOS syscall as a guest's.

Hook points, and what each covers:

| site | covers |
| --- | --- |
| `KernelMMap` / `KernelMMapFixed` / `KernelMUnmap` | Junction's deliberate anonymous mappings |
| `KernelMProtect` / `KernelMAdvise` | protection and advice changes |
| `KernelFile::MMap` / `MMapFixed`, `LinuxFile::MMap` | file-backed mappings |
| `syscall_trap_handler` | Junction's **own glibc**, trapped by seccomp |
| `usys_mmap` | guest-initiated mappings |
| `on_runtime_map()` (weak hook in Caladan) | `mem_map_anom`, `stack_create`, `lgpage_create` -- i.e. pool overflow |

Not covered: the startup memfd sweep in `InitAddressSpaces()`, which uses raw
`ksyscall` and runs before tracing starts. It is init-time only and its output
is visible in `/proc/self/maps`.

`--debug_libos_alloc <MB>` is a deliberate provocation: on the first guest
`getpid()`, the LibOS allocates that much through its own glibc while a guest
is the current address space, so the dangerous case can be observed rather than
inferred from its absence.

## What the measurements found

Five workloads, traced end to end.

| workload | events | LibOS-range | Caladan | Junction libc |
| --- | --- | --- | --- | --- |
| `openssl speed` | 19 | 0 | 0 | 0 |
| `python3` (json/re/hashlib, 32 MB buffer) | 124 | 0 | 0 | 0 |
| `python3`, heavier | 160 | 0 | 0 | 0 |
| `fork_stress -t 4 -s 2 -f 32` (~16k forks) | 121 | 0 | 0 | 0 |
| `fork_stress -t 16 -s 3 -f 512 -k 4096` | 148 | 0 | 0 | 0 |

Operation mix across all five: 309 `mmap`, 100 `mprotect`, 74 `munmap`, 60
file mappings, 29 `madvise`. **Every one landed in the guest range.** The 194
`mmap`s attributed to "junction" are Junction acting *for* a guest — `usys_mmap`
down through `MemoryMap` into `KernelMMap` — not the LibOS acquiring memory for
itself.

So in steady state Junction does not allocate for itself at all, and the
pre-allocated Caladan pools never overflowed even at 512 concurrent guests. The
guest-side behaviour is otherwise unremarkable and entirely as expected: Python
does 36 file mappings, **9 of them executable** (`prot=0x5`) — real `dlopen`s of
extension modules — and the `mprotect`s are the loader relocating and then
re-protecting each object.

Zero is a useful number but a weak one, so the case was also provoked.

### Provoking it: `--debug_libos_alloc`

Forcing the LibOS to `malloc` through its own glibc, from a guest syscall
handler, in a forked child's address space, found **two** distinct failures.

**1. It segfaults before it ever reaches `mmap` — on TLS, not on memory.**

```
Thread 1 received signal SIGSEGV
0x00007ffff78ad7dd in __GI___libc_malloc (bytes=1048576) at malloc/malloc.c:3334
=> 0x7ffff78ad7dd <__libc_malloc+141>:  lock cmpxchg %edx,(%r12)
$fs_base = 0x4ffffddb7740
#1  junction::usys_getpid () at junction/kernel/proc.cc:772
```

`fsbase` is `0x4ffffddb7740` — below `kVirtualAreaMax`, i.e. **the guest's
TCB**. Junction never restores its own `fsbase` on the syscall entry path;
`SetFSBase(perthread_read(runtime_fsbase))` appears only in `signal.cc`, on the
paths back to the Caladan scheduler. glibc's `malloc` reads `thread_arena` and
`tcache` from TLS, so Junction's glibc reads *the guest glibc's* TLS layout,
gets a garbage arena pointer, and dies on the arena lock.

This is a stronger constraint than the address-space one, and it explains the
column of zeros above: Junction's LibOS is *structurally* prevented from calling
its own glibc in guest syscall context. Not rare — impossible.

**2. With `fsbase` corrected, the discriminator misfires, and then the memory
lands in a guest.**

With the LibOS `fsbase` restored, `malloc` proceeds and issues `brk`:

```
Unexpected syscall while in_kernel (brk)
```

Preemption is *enabled* in a guest syscall handler, so `!preempt_enabled()` is
false and the trap handler routes the LibOS's own `brk` to the **guest** syscall
table. Exactly the misclassification predicted above, on the first try, in the
simplest possible case.

Disabling preemption around the allocation — giving the heuristic what it needs
to be right — lets it complete, and then:

```
debug: guest pid 2 as=4294967297 fsbase=0x4ffffddb7740 preempt_enabled=1
debug: LibOS malloc(128 MB) -> 0x7bc0b3fdf010 [libos-range] in address space 4294967297
debug: address space 0 looking for the LibOS mapping made in address space
       4294967297 at 0x7bc0b3fdf010: ABSENT -- the LibOS allocated memory that
       this address space cannot see
debug:   control (LibOS text at 0x61a2bfe35cc0, mapped before the fork): PRESENT
```

Reproducible 3/3. The control — LibOS text mapped before any fork — is `PRESENT`
from the same probe in the same run, so this is Junction's behaviour and not the
probe's.

Read via `/proc/thread-self/maps`, not `/proc/self/maps`: `self` resolves to the
thread group leader, whose `mm` is the root one, and it reports `PRESENT` for
every address regardless of which address space the calling kthread is bound to.
A convenient way to fool yourself.

### Telling the two glibcs apart, settled

`fsbase` is not a protection mechanism, so it settles less than it first
appeared. On x86-64 the FS segment has a 64-bit base held in the `FS_BASE` MSR;
an `fs:`-prefixed access computes `fsbase + offset`. It is how thread-local
storage works: glibc points it at the thread control block, where `errno`, the
stack canary, `pthread_self()`, and malloc's `thread_arena` and `tcache` live.
It is an addend, not a bound. It restricts nothing.

So the crash above is not a safety property. It is Junction failing loudly, by
accident, on one path.

The trace attributes at the call site, but the production discriminator has to
decide from the trap. Three candidates:

| discriminator | verdict |
| --- | --- |
| `!preempt_enabled()` | **unsound.** Wrong on the very first forced case. |
| trapping RIP vs `kVirtualAreaMax` | **sound.** |
| `fsbase` vs `kVirtualAreaMax` | **unsound: guest-controlled.** |

RIP works because the two glibcs are two distinct images at ranges the slot
partitioning already separates — Junction's at `0x7ffff78ad000`, the guest's at
`0x4ffffddba000` from `install/lib/libc.so.6` — and a guest cannot execute above
`kVirtualAreaMax` because `usys_mmap` confines it below.

`fsbase` cannot be used, because `usys_arch_prctl` does not validate it:

```c
long usys_arch_prctl(int code, unsigned long addr) {
  if (code != ARCH_SET_FS) return -EINVAL;
  thread_set_fsbase(thread_self(), addr);   // no range check
  return 0;
}
```

A guest can set `fsbase` to any value it likes. If the trap handler classified
on `fsbase`, a guest would set it above `kVirtualAreaMax` and have its own
`mmap`s served out of the LibOS arena — which is shared across guests. That
turns a correctness heuristic into a sandbox escape. RIP it is, unless
`arch_prctl` starts range-checking, and even then RIP is free.

### Can anything escape the arena? Yes — six for six

`--debug_libos_escape` runs a battery from a genuine LibOS context against a
nominal arena slot at `0x530000000000`:

```
malloc(64 MB)                    -> 0x639a7c0b1890   OUTSIDE  [trapped: redirectable]
posix_memalign(2 MB, 8 MB)       -> 0x747c10600000   OUTSIDE  [trapped, but glibc trims with munmap]
ksys_mmap(NULL, 1 MB)            -> 0x747c3a617000   OUTSIDE  [NOT trapped]
ksys_mmap(NULL, exec page)       -> 0x747c3ac1d000   OUTSIDE  [NOT trapped, and PROT_EXEC]
ksys_mmap(hint past arena, 1 MB) -> 0x538000000000   OUTSIDE
dlopen(libm.so.6)                                    OUTSIDE
```

The first two are fine — they trap, so a redirect places them. **The rest are
the problem: `ksys_mmap`'s RIP is inside the `ksys` range, which is exactly the
range the seccomp filter allows, so it never traps.** A redirect in the SIGSYS
handler cannot see Junction's own allocations at all. There are about fourteen
such call sites (`perf.h`, `memfs.cc`, `mm.cc`, `syscall.cc`, `jif.cc`,
`shim/backend/init.cc`, `zpoline.cc`, `linuxfile.cc`).

Two consequences:

- The arena needs a second enforcement point inside `KernelMMap`/`ksys_mmap`,
  not just the trap. The trap covers glibc; the wrapper covers Junction.
- `InitZpoline()` maps `PROT_EXEC|PROT_WRITE|PROT_READ` at a kernel-chosen
  address. It is init-time today, so it predates every fork, but it is a
  standing example of the executable-mapping case an RW arena cannot serve.

### The bug this actually found: memfs extents

The measurement had a hole. The tracer hooked the `KernelMMap` wrapper, and
`memfs.cc` calls `ksys_mmap` directly, so none of it was recorded. With that
closed, a trivial Python script that writes eight files produces eight LibOS
mappings that the first round reported as zero:

```
mmap-memfs-extent junction 0x380000000000 268435456 prot=0x3 flags=0x1 fd=9
mmap-memfs-extent junction 0x380010000000 268435456 prot=0x3 flags=0x1 fd=9
... one 256 MB extent per file
```

`MemInode::Create` maps a fresh 256 MB extent per file, at a monotonically
increasing address, **lazily, whenever a file is created**, into whichever
address space is current. That is precisely the failure class this document is
about, and it is reachable by an ordinary program:

```c
pid_t c = fork();                       // child's address space is a snapshot
if (c == 0) { wait_for_parent(); read("/tmp/f"); }
create("/tmp/f");                       // new extent, mapped in the PARENT only
```

Native Linux: `child: read 4096 bytes, first=A`. Junction: segfault, 3/3.

```
FAULT ADDR: 0x380000000000
#0  __memcpy_avx512_unaligned_erms ()
#2  junction::memfs::MemInode::Read (this=0x510000059410) at memfs.h:75
#3  junction::memfs::MemFSFile::Read () at memfsfile.h:20
#4  junction::usys_read () at fs/file.cc:220
```

The fault address is exactly the extent the trace recorded being mapped into
`as=0`. `--no_mas` cannot serve as a control, because without multiple address
spaces there is no `fork()` — this is a gap in the new work, not a pre-existing
Junction bug.

### memfs is already the design

The mechanism this document proposes building is already in the tree. `memfs.cc`
creates one sparse memfd at init and slices it:

```c
memfs_extent_fd = memfd_create("memfs", MFD_FLAGS);   // MFD_FLAGS == MFD_EXEC
ftruncate(memfs_extent_fd, kMaxMemfdExtent);          // 35 TB sparse
```

`kMaxSizeBytes` is 256 MB, so 128K slots. Every memfs file is
`mmap(nextp, 256 MB, RW, MAP_SHARED, memfs_extent_fd, slot * 256 MB)`.

Three things follow, and two of them revise this plan.

**`MFD_EXEC` weakens the executable-mapping objection.** Above, an RW arena was
said not to be able to back executable LibOS mappings, making that the case that
justifies a window VMA. But memfs already asks for `MFD_EXEC` (falling back only
if the kernel refuses), and protections are per-mapping — `scratchpad/memfd_probe.c`
verified RX and RW mappings of one memfd coexisting. An `MFD_EXEC` arena can
serve `PROT_EXEC` with W^X intact. The window VMA is less necessary than
claimed.

**The address is not the offset.** The plan assumed `offset = addr - slot_base`,
so propagation needs no bookkeeping. memfs does not work that way: the address
comes from `next_memfs_faddr.fetch_add(kMaxSizeBytes)`, monotonic and never
reused, while the offset comes from `allocated_file_slots.find_next_clear(0)`, a
recycled bitmap. Measured over 40 create/delete cycles of one file:

```
distinct memfd offsets: 1      <- slot correctly recycled
distinct addresses:    40      <- 0x380000000000, +256 MB each time
```

Propagation must carry `(addr, offset)` pairs rather than derive one from the
other. The author flagged the allocator already:
`// Temp hack for memfs serialization/loading with elf.`

**Teardown propagates exactly half.** `~MemInode` does `MADV_REMOVE` then
`KernelMUnmap`. The `MADV_REMOVE` punches a hole in the memfd, which the kernel
propagates to every mapping through `i_mmap` — free, as predicted. The
`KernelMUnmap` only unmaps in the current address space, so every other address
space keeps a 256 MB mapping over the hole. Combined with the recycling above, a
stale mapping points at a slot that now belongs to a *different file*. Whether a
forked child can still reach `buf_` for a freed inode depends on inode lifetime
across the fork and has **not** been shown; it is the shape of an
information-disclosure bug and needs checking before this area is called done.

Starting at `0x380000000000` and burning 256 MB per creation,
`next_memfs_faddr` reaches `kVirtualAreaMax` after 6144 creations. Not a
clobber — the mmap hint is advisory and the kernel just places the extent
elsewhere — but the addresses stop being predictable there.

memfs is the smallest useful first target for step 4: a demonstrated failure
with a test case, already memfd-backed, so it exercises propagation without
also needing the arena.

### What this changes about the plan

- The discriminator becomes trapping RIP. Not `fsbase` (guest-settable) and not
  preemption state (already misroutes `brk`). This is a correctness fix.
- **The redirect needs two enforcement points, not one.** The SIGSYS trap covers
  glibc; `ksys_mmap` is allowed by the filter and needs its own. Any design that
  relies on the trap alone is incomplete by about fourteen call sites.
- **Do memfs first.** It is a demonstrated, reproducible failure with a
  test case, and it is already memfd-backed, so it exercises the propagation
  machinery without also needing the arena.
- The `fsbase` problem is prior to the arena and independent of it: an arena the
  LibOS cannot call `malloc` to reach is not much use, so the redirect must
  either restore `fsbase` or avoid glibc entirely.
- Executable LibOS mappings are no longer purely hypothetical — `InitZpoline()`
  makes one. It is init-time, so it is not yet a bug, but it is the case that
  bounds what an RW arena can cover.

## Can every LibOS allocation be confined to the arena?

Yes, for correctness. It needs three enforcement points, and the set of paths is
closed — every way the process can obtain memory is one of these.

### 1. Junction's own call sites — edit them

`ksys_mmap`'s `syscall` instruction sits inside `[ksys_start, ksys_end)`, which
is exactly the range the seccomp filter allows, so these **never trap**. This is
measured, not assumed: `--debug_libos_escape` calls `ksys_mmap` directly and it
succeeds with no SIGSYS event. No interception is possible; the ~14 call sites
have to be changed by hand to route through the arena allocator. Mechanical, and
`grep ksys_mmap\|KernelMMap` bounds the work.

### 2. Junction's glibc — redirect at the trap

Its `mmap` is outside the ksys range, so it traps, and the SIGSYS handler is the
choke point. This is where the discriminator has to be right (below). Caladan
reaches the kernel through libc `mmap` too, so pool overflow lands at the same
point — not separately measured, because the pools never overflowed.

### 3. `brk` — already dead

The handler forces `rax = 0`, so glibc's main arena cannot grow and everything
goes to mmap. That is what makes (2) a complete cover for glibc.

### Telling LibOS from guest: use RIP or RSP

Each guest gets its **own** glibc — `install/lib/libc.so.6`, loaded by
`install/bin/ld.so`, mapped below `kVirtualAreaMax` (observed at
`0x4ffffddba000`). The LibOS uses the *host* glibc linked into `junction_run`
(observed at `0x7ffff78ad000`). Two distinct images on opposite sides of the
partition, so:

- **Trapping RIP** — which glibc image is executing. Direct answer to the actual
  question.
- **Trapping RSP** — which stack. Also works, because `junction.cc:230` reads
  `if (mas_enabled_) stack_switching = true;`, so under multiple address spaces
  LibOS code *always* runs on a LibOS stack. Guest stacks are below
  `kVirtualAreaMax`, Caladan's are at `0x520000000000`.

Either is sound; cheapest is to check one and assert the other agrees, which
costs nothing and catches surprises. Not `fsbase` (guest-settable via
`usys_arch_prctl`) and not `!preempt_enabled()` (already misroutes `brk`).

**Caveat: none of these is unforgeable against a hostile guest.** LibOS text and
stacks are mapped in every guest address space — that is the point of sharing
them — so a guest can jump into LibOS text or point RSP at a LibOS stack and
have its syscall classified as the LibOS's. That is a property of Junction's
existing single-address-space-per-guest design rather than something the arena
introduces, and it is the same reason a guest can already read LibOS state. The
discriminators are sound for *correctness*; treating them as a security boundary
needs a separate argument.

### Does the memfd enforce its own size?

Measured (`scratchpad/memfd_bounds.c`), 16 MB memfd:

```
mmap inside  (off=0)         ok   -> write OK
mmap at EOF  (off=16M)       ok   -> FAULTED (SIGBUS)
mmap past EOF (off=64M)      ok   -> FAULTED (SIGBUS)
mmap straddling EOF          ok   -> first page OK, page past EOF FAULTED
F_SEAL_GROW|F_SEAL_SHRINK    sealed; later ftruncate refused
```

So the kernel enforces the bound **at fault time, not at mmap time**. `mmap`
succeeds four times past the end. That makes the size a backstop — nothing can
silently allocate outside it — but useless as an allocator: you cannot use
"mmap failed" as the out-of-space signal, and a leak shows up as a SIGBUS in
unrelated code. Seal the size with `F_SEAL_GROW | F_SEAL_SHRINK` so it cannot be
grown by accident.

### So we allocate offsets ourselves — with code that already exists

`ExclusiveIntervalSet<T>::FindFreeRange(hint, len, upper_lim, lower_lim)` in
`junction/base/interval_set.h` is already generic. It backs both the global
region map (`mem_areas_`, bounded by `kVirtualAreaMax`) and each process's
`vmareas_`, and `AllocateMMRegion()` is a four-line example of exactly the
wanted shape:

```c
Status<uintptr_t> ret = mem_areas_.FindFreeRange(0, len, kVirtualAreaMax, 0);
if (ret) mem_areas_.Insert({*ret, *ret + len});
```

One instance over `[kArenaBase, kArenaBase + 512 GB)` is the arena allocator.
Because the arena's addresses and the memfd's offsets are 1:1 by construction,
`offset = addr - kArenaBase` holds, there is a single allocator rather than two,
and propagation stays the one-liner the plan assumed.

**This also fixes memfs.** memfs today runs a monotonic address allocator
(`next_memfs_faddr`, never recycled, 6144 creations to reach `kVirtualAreaMax`)
*and* a separate recycled offset bitmap, which is what breaks the
address↔offset identity and strands mappings in other address spaces. Replacing
both with one interval set over a reserved slot fixes the address leak and
restores the identity, using code memfs's own neighbours already use.

## Startup mappings, Caladan, and the networking worry

### MAP_SHARED needs nothing

The worry list — iokernel control regions, SysV shm, hugetlbfs packet buffers,
NIC queues — mostly dissolves, because a `MAP_SHARED` mapping is inherited by
`clone()` as a mapping of *the same object*. Every address space sees the same
pages and the same writes, with no propagation and no bookkeeping. This is
already the load-bearing invariant of the existing design ("all LibOS writable
memory must be `MAP_SHARED`"), and the sweep exempts anything already shared:

```c
if (!m.writable || m.shared) return true;   // IsExemptFromSharing
```

The arena exists for exactly one thing: giving *anonymous private* memory a
name, so it can be re-mapped into another address space. Anything with an fd
already has a name.

The one distinction to keep straight is **coherence versus presence**. Shared
mappings are coherent for free, but a shared mapping created *after* a clone is
still absent from the older address space — no VMA there. So new shared
mappings still need propagating; they just need no new mechanism to do it, since
`mmap(addr, len, prot, MAP_SHARED|MAP_FIXED, fd, off)` with the original fd is
enough. This is what memfs gets wrong, and it is why memfs fails while the
iokernel regions do not: the iokernel regions are all mapped before the first
guest exists.

### Two phases, two mechanisms

The question "should the memfd be created before the Caladan runtime starts?"
has a better answer than yes: keep the two phases separate.

- **Init.** Let everything initialise normally, then sweep `/proc/self/maps`
  once, before the first guest, converting private-writable to shared. Already
  built (`ShareLibOSMemory`), already verified, and robust in a way interception
  is not — a sweep catches mappings nobody remembered, and `AuditLibOSMemory()`
  reports what it could not convert.
- **Steady state.** Arena plus redirect, for allocations made after that point.

Trying to route init-time allocations through the arena would mean intercepting
Caladan's startup, which is both harder and less trustworthy than reading the
map afterwards and checking.

### What the live map actually contains

126 mappings, a guest running:

| | count | size | |
| --- | --- | --- | --- |
| `SHARED w` `memfd:junction_libos` | 27 | 69 MB | the sweep's output |
| `SHARED w` anon (`/dev/zero`) | 21 | 17.4 GB | relocated Caladan stack + page pools |
| `SHARED -` SysV shm | 2 | 512 MB | iokernel info, ingress mbufs |
| `SHARED w` hugetlbfs | 1 | 2 MB | |
| `SHARED w` memfd | 1 | 66 MB | memfs extent fd |
| `private -` anon | 12 | 66 GB | `PROT_NONE` address-space reservations |

**Private and writable, the only kind that diverges on clone: nine mappings,
seven of which are the guest's own** (its glibc data, heap, stack, binary). The
two in the LibOS range are both accounted for:

- `7ffbd7c83000-7ffbd7c93000`, 64 KB — the clone stack from
  `InitAddressSpaces()`, `MAP_PRIVATE|MAP_ANONYMOUS`, which the audit explicitly
  skips. Used transiently by the thread calling `clone()`, whose child is killed
  immediately, so divergence is harmless — but it is an exception that should be
  justified in code rather than skipped silently.
- `[stack]`, 132 KB — the initial thread's stack, exempt by design (the main
  thread parks in the Caladan scheduler and runs on runtime stacks thereafter).

So the invariant already holds at steady state, with two documented exceptions.

### Caladan's runtime mapping sites: there are two

Every `mmap`/`mem_map_*` call site in `lib/caladan/runtime` and
`lib/caladan/base` sits inside a function named `*_init*`, with exactly two
exceptions:

| site | function | status |
| --- | --- | --- |
| `runtime/stack.c:67` | `stack_create` | pooled, pre-reserved, hooked |
| `base/page.c:122` | `lgpage_create` | pooled, pre-reserved, hooked |

`iok_shm_alloc` looks like a runtime allocator but maps once
(`if (!r->base)`) and every caller is `ioqueues_init`, `storage_init` or
`mlx5_init_verbs`; afterwards it is a bump allocator inside that one region.
The directpath mappings are both in `mlx5_init_ext_late`.

So "hard-to-reason-about Caladan code" is bounded to two pooled allocators that
are already instrumented and already pre-reserved, and the `on_runtime_map()`
hook fires if either pool ever overflows. Across five workloads it never did.

### The gap: directpath was disabled in every measurement

Every trace so far ran under `WARNING: DIRECTPATH DISABLED`. This machine has a
ConnectX-7 (`mlx5_0`), but the iokernel was started with a TAP vdev
(`--vdev=net_tap0`), so the mlx5 paths never executed. The static reading says
they are init-time only, and `mlx5_init_ext_late` is an init function — but that
is a code reading, not a measurement, and it is precisely the area where a code
reading is least convincing.

Re-running with directpath requires restarting the iokernel bound to the
physical NIC, which is disruptive enough to be a deliberate choice rather than a
side effect of a test. Until then, treat the networking conclusion as
provisional.

## Structural divergence: the silent failure mode

`MAP_SHARED` makes page *contents* coherent for free. It does nothing for the
mapping *structure* — which virtual address maps which object at which offset
with which protections. That is per-mm, and every operation that changes it
diverges.

This splits the problem in two, and the halves want different solutions:

| | example | symptom |
| --- | --- | --- |
| **Absence** | new mapping, not yet in this mm | **loud** — faults |
| **Divergence** | `mremap`, `munmap`, `mprotect`, `MAP_FIXED` over a live range | **silent** — VA valid in both mms, different meaning |

The second is strictly worse and no fault handler can catch it: there is nothing
to fault on. `mremap`ping `0x1000-0x6000` to `0x2000-0x7000` in one mm leaves
every other mm with a perfectly valid `0x2000` holding entirely different bytes.

### Why not an operation log

A global op log with per-mm epochs, replayed on switch, is complete and easy to
reason about. It also costs the things this design exists to protect:

- Replay sits **on the mm-switch path**, the one path that has to stay cheap.
- The backlog is unbounded per mm. An address space that sits idle accumulates
  entries, so its *next* switch is a latency spike — precisely the tail
  behaviour Caladan is built to avoid.
- It needs GC (truncate once every mm has caught up), which needs a registry of
  live mms and a story for idle and dying ones.
- Replay has to be idempotent and correctly ordered against concurrent faults.

Worth building only if the silent class is non-empty. It does not have to be.

### Making the silent class empty

Three properties, and the log becomes unnecessary:

**1. 1:1 address↔offset.** `offset = addr - slot_base`. Then a stale VMA at
address X maps offset `X - base` — which is exactly what any *new* occupant of X
uses. A stale mapping is not wrong, it is early. Reading through it gives the
same bytes a single address space would have given, so the behaviour degrades to
exactly the non-MAS case rather than to a new failure mode.

This is the property memfs violates — monotonic addresses, recycled offset
bitmap — and it is why memfs is the one thing that actually breaks.

**2. Free by punching, never by unmapping.** `munmap` is local and diverges.
`fallocate(PUNCH_HOLE)` / `MADV_REMOVE` is global: the kernel walks `i_mmap` and
zaps every mapping of the range everywhere, with the shootdowns. So the arena
allocator recycles ranges internally and never returns a VMA to the kernel;
freeing memory means punching the hole and keeping the mapping. Verified in
`scratchpad/memfd_probe.c`.

**3. Uniform protection per slot.** Then `mprotect` never happens on arena
memory. If RX is ever needed (LibOS `dlopen`), use a *second* slot that is
uniformly RX rather than mixing protections within one — so the address still
determines offset *and* protection.

With those three, the only structural change left is "a new range became valid",
which is the loud class, served lazily by the fault handler: idempotent, no log,
no epochs, no GC, nothing on the switch path.

### Where the "stale VMAs are harmless" argument actually stands

That argument was stated too strongly above. Precisely:

**What survives.** Under *strict* 1:1 there is no "old layout" and "new layout"
for a pointer to be interpreted under — address → offset is a global constant
with no versions. A pointer to X means offset `X - base` in every mm, always.
The only way two mms can disagree is *presence*, which faults and is served
lazily. So inside a region where 1:1 genuinely holds, the scenario cannot arise.

**What does not.** 1:1 is a global invariant, and the failure when it is
violated is much worse than "early". Because content is shared and coherent, a
stale mm reads *fresh* data — data written by another mm under a layout
assumption the stale mm does not satisfy. Pointers in that fresh data (a runtime
stack, a linked structure, an allocator freelist) then resolve to the wrong
object. The staleness does not stay local to the mm that is behind; it is
actively propagated to it through the shared pages. One violation anywhere
poisons every mm that has not caught up.

So the three properties do not remove the need for a log *in general*. They
remove it *within a region where they are mechanically enforced*. Enforcement,
not intention, is what the argument rests on — and the plan above had intention.

**A violation that is already reachable.** glibc relocates large chunks on
`realloc`:

```
$ strace ./realloc_probe
mremap(0x7f692f3ff000, 4198400, 16781312, MREMAP_MAYMOVE) = 0x7f692e3fe000
moved: yes
```

Step 3 as sketched intercepts `mmap` only, so the first large `realloc` from
Junction's own glibc moves a mapping in one address space and breaks 1:1
silently. The redirect must therefore cover **`mremap` and `munmap` as well as
`mmap`**, and must implement `mremap` as allocate-copy-punch rather than move,
so that no operation can ever change what an address means. That is the
mechanism that makes 1:1 an enforced property instead of a hope.

The general rule the arena has to guarantee: *no operation may change the object
or offset that a given virtual address refers to.* Growth allocates elsewhere
and copies; freeing punches; nothing moves.

### What this does not cover

**Regions that cannot be 1:1.** memfs, hugetlbfs pools, the iokernel shm
regions and the SysV segments each have their own address↔object relationship
and cannot all be folded into one arena. For those the safety argument is not
1:1 but *frozen after init* — nothing structural changes once the first guest
exists. That is a different invariant and needs its own enforcement.

If any region ever legitimately needs to relocate at runtime, neither argument
covers it, and that region needs eager propagation at the operation or the
op-log after all. The log is not dead; it is the escape hatch, and the design
should be honest that it becomes load-bearing the moment such a case appears.

**Mappings outside the arena.** The init-time shared regions (iokernel, shm,
hugetlbfs, memfs extents) rely on never being structurally changed at runtime.
That holds today — LibOS `mremap` has exactly two call sites, `InitZpoline()`
and the vdso relocation in `syscall.cc:72`, both init-time, and every
`KernelMProtect` site is guest-directed. But it holds by accident, not by
construction.

Cheap fix: assert it. A check in `KernelMRemap` and `KernelMProtect` that the
target is either a guest range or that init has not finished turns a future
silent bug into a loud one at the call site, for the cost of a comparison.

**Detection for everything unforeseen.** Where a log-shaped thing does earn its
place is off the critical path: extend `AuditLibOSMemory()` from "is anything
private and writable" to "do all live address spaces agree on the LibOS range",
by diffing `/proc/thread-self/maps` per address space. Debug-mode only, zero
steady-state cost, and it catches divergence of any origin including the ones
this document failed to predict.

## Shadow VMAs as ground truth: state, not log

Banning `mremap` was too strong. In an ordinary single-mm process, `mremap`ping
a region a linked list points into breaks the list too — that is the caller's
bug, not the operation's. There is no reason the LibOS cannot `mremap`, provided
every mm can find out that it happened.

**The key move is state rather than history.** Because the memfd's contents
persist independently of any mapping, an mm that is behind does not need to
replay what happened; it only needs to make its view match the current shadow
map. That is idempotent and order-free: no epochs for correctness, no GC, no
unbounded log, and no replay cost proportional to backlog. It removes every
objection raised against the op-log above.

So the design is: one global `ExclusiveIntervalSet<ShadowVMA>` recording
`VA range -> (fd, offset, prot)` for the whole LibOS range, and every mm syncs
its page tables to it.

### The price of allowing mremap

With strict 1:1 and no moves, the fault handler alone is sufficient and the
generation counter is a pure optimisation: the only possible difference between
mms is *absence*, absence faults, and the fault is served by arithmetic.

Allowing moves changes that, and it is worth being explicit about it:

- **Additions stay self-correcting.** Absent -> fault -> consult the map -> map
  it. Lazy, no coordination.
- **Removals and moves are not.** After mm A `mremap`s X to Y, mm B still has X
  mapped and nothing faults. B reads the old bytes through X — which in a
  single-mm process would have faulted, because there X is unmapped. B is
  therefore *more permissive* than a normal process: a use-after-move that ought
  to crash silently succeeds.

So sync-on-switch becomes **load-bearing for correctness**, not just latency: it
must run before the mm executes anything, and it must apply removals, which no
fault can trigger. That is the real cost of allowing `mremap`, and it should be
a deliberate trade rather than something discovered later.

A reasonable synthesis: keep 1:1 as a **fast path, not a requirement**. The
shadow map is ground truth and covers everything — moves, and memfs's arbitrary
address/offset pairing. But wherever `offset == addr - base` holds, the fault
handler skips the lookup and uses arithmetic. Common case (the heap) needs no
shared structure at all; only relocated or non-1:1 ranges consult the map.

### Allocating the shadow map

Two real constraints, and one worry that turns out not to be real.

**RSS does not multiply.** There is one process. Every mm maps the *same* memfd
pages, so physical memory is one copy and the alternate page tables are the only
per-mm cost. Reserving capacity costs address space, and a sparse memfd costs
nothing physical until touched. "Every LibOS instance grows by the size of the
structure" does not happen, because there are no instances — just alternate
views of the same pages.

**Bootstrap is the real constraint.** The fault handler consults the map, so a
fault while consulting the map recurses. The node pool must therefore be
*eagerly* mapped in every new address space, never lazily. That is what forces
a fixed, pre-reserved region — not fragmentation.

It is affordable. The live map has **102 LibOS-range mappings** at steady state.
At 100x headroom, 10K nodes of ~64 bytes is ~640 KB, about 160 pages to
pre-fault per address space: sub-millisecond, one-time, against a ~142 us fork.

**Fragmentation is a non-issue**, for a reason worth stating: `std::map` nodes
are all *one size*. A freelist over a fixed array of nodes is a slab, not a
general allocator, so there is nothing to fragment. That is why "reserve a
maximum capacity" works here when it would not for a general heap.

**Pointers inside the structure** must be valid in every mm, which they are as
long as the pool sits at a fixed address — which it does, being in the arena.

Junction already has the container: `ExclusiveIntervalSet<T>` wraps
`std::map<uintptr_t, T>` and already backs `vmareas_` and `mem_areas_`. Give it
a pool allocator over a pre-reserved, eagerly-mapped region and it is the shadow
map.

## Propagation, settled: quarantine makes lazy correct

Lazy propagation fails only because of **address reuse**. Every silent case —
`mremap` into a range a stale mm still maps, `munmap` followed by reallocation —
requires that an address mean one thing in one mm and something else in another.
Forbid reuse and the whole class disappears:

> **Invariant.** An address range is never reused for a different
> `(fd, offset, prot)` binding until it has been unmapped from every mm that
> materialised it.

Given that, an address in any mm is in exactly one of three states:

1. the current, correct binding;
2. **no binding** — faults, and the fault handler consults the shadow map and
   fixes it, lazily and idempotently;
3. a stale binding **to the same `(fd, offset)` it always had** — right content,
   possibly outliving its logical lifetime.

State 3 is the only looseness and it is strictly *more permissive* than a normal
process (a use-after-move that would fault there silently succeeds here). It is
never wrong data. There is no fourth state, so there is nothing to propagate
eagerly and nothing to do on an mm switch.

This is the "refcount shadow VMAs so remaps always hit unused address space"
idea, and it is the right one. Address space is the cheap resource here: one
512 GB slot, and more slots available.

### The shootdown list: the kernel already keeps it

The worry about maintaining a userspace list of mms to invalidate — "the LibOS
can never miss an operation" — dissolves, because the list should not be
maintained in userspace at all. `fallocate(PUNCH_HOLE)` walks the memfd's
`i_mmap`, which *by construction* contains every mm that maps it, zaps the PTEs
and flushes each mm's TLB. Measured in `docs/traces/tlb_punch.c`. You cannot
miss an mm because you are not the one tracking them.

The two mechanisms are complementary and neither visits another mm:

| | reclaims | mechanism | cost |
| --- | --- | --- | --- |
| `PUNCH_HOLE` | physical memory, everywhere, with shootdowns | kernel `i_mmap` walk | one syscall |
| quarantine | nothing yet — just refuses to reuse the address | allocator policy | a VMA (~200 B) per stale mm |

Punch alone does **not** restore faulting: the VMA survives, so a stale mm reads
zeros rather than faulting. Quarantine is what guarantees nothing ever points
there. Together: free and `mremap` become a local operation plus a punch, with
no eager propagation and nothing on the switch path.

### GC is the only thing that ever touches another mm

Eventually quarantined ranges should be recycled: visit the mms holding them,
`munmap`, return the range to the free list. That is the one operation that
walks other address spaces, and it is batched, rare, and off every critical
path.

Two measurements say it is close to free in practice:

- **LibOS-range allocation events after init: 0**, across all five workloads.
  The propagation machinery is a cold path.
- `max_map_count` on this machine is **16777216**, against 102 live LibOS
  mappings, so accumulated VMAs are not a practical bound.

An optimisation, if GC ever shows up in a profile: give each shadow VMA a
membership bitmap, set by the fault handler when an mm materialises it. GC then
visits the *k* actual sharers instead of every live address space. Not needed
for correctness — add it when measured.

### The guest-buffer case does not arise

LibOS buffers allocated on behalf of an application are not in the arena: guest
mappings live below `kVirtualAreaMax`, are per-mm by design, and `MemoryMap`
already owns them. One mm cannot relocate another guest's buffers because it has
no name for them. The case only exists for a LibOS-owned region a guest holds a
pointer into — a shared ring, say — which is a deliberate interface with its own
contract, not an accident of the allocator.

### Where the three open items stand

1. **Bootstrap.** Two phases. Init: let everything start, then sweep
   `/proc/self/maps` and audit — already built and verified. The change is to
   have the sweep consolidate onto *one* arena memfd (it currently creates 27,
   one per mapping) and record each range into the shadow map as it goes, so the
   map is seeded by the same pass that establishes the invariant.
2. **Call sites versus interception.** Both, because they cover disjoint sets
   and this is measured, not assumed: `ksys_mmap` never traps (its syscall
   instruction is inside the filter's allowed range), so those ~14 sites must be
   edited; glibc and Caladan always trap, so those are intercepted at the SIGSYS
   handler, discriminating on trapping RIP. Add an assertion in
   `KernelMRemap`/`KernelMProtect` so a future call site fails loudly instead of
   silently.
3. **Propagation.** Quarantine plus lazy fault-in. Nothing eager, nothing on the
   mm-switch path, and the kernel does the shootdowns.

## Constraints to hold on to

- **Bootstrap.** Whatever the fault handler itself touches must be eagerly
  mapped — its stack, the mirror, the lock. You cannot lazily fault in the
  machinery that handles faults.
- **Concurrent faults.** Two cores can fault the same range and both
  `MAP_FIXED` it. Same memfd, same offset, same prot, so it is benign, but the
  transient remap could make a third core fault. A per-address-space propagated
  bitmap under a lock in the mirror closes it; it is the slow path.
- **Executable LibOS mappings** created after a fork (a `dlopen` bringing in
  text) cannot be served from an RW arena. Junction does not do this today. If
  it ever must, that is the case that justifies a window VMA, and it should be
  built for that case deliberately.
- **No module build is loaded on the host** until `scripts/vm_test.sh` passes
  for that exact build.
