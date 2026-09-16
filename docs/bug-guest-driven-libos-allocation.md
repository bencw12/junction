# Bug: a guest program can crash Junction by opening pipes

**Status:** open. Reproducer and regression test landed; fix is Phases 1-3 of
`docs/fork-implementation-plan.md`.

**Reproduce:** `scripts/pipe_heap_test.sh` — 3/3, stock config, no debug knobs,
no LibOS modification.

```
page: growing the heap past the reserved pool; new memory is visible only in
      the address space that allocated it
parent: opened and closed 17000 pipes (1062 MB of pipe buffer)
fault with preemption disabled: addr=5100444a0000 rsp=520000cf9e50
                                pid=2 as=4294967297 want_as=4294967297
Aborting on signal: signal delivered while preemption is disabled
```

`addr=0x5100444a0000` is inside the Caladan large-page pool
(`PAGE_BASE_ADDR 0x510000000000`) and past the 1 GB reservation that ends at
`0x510040000000`. `pid=2` is the forked child, in its own address space, reading
LibOS memory that address space never had mapped. `want_as == as`, so the
address-space switch was correct — the mapping simply is not there.

Deterministic: the fault address is `0x5100444a0000` in all three runs.

## The chain

Two facts have to be put together, and neither is visible from the other's file.

**1. Junction's C++ allocations do not go to glibc.** `junction/new_override.cc`
overrides global `operator new`:

```c
constexpr size_t kMaxAllocSize = (1UL << 18);        // 256 KB
if (unlikely(size > kMaxAllocSize)) return std::malloc(size);
return smalloc(size);
```

Everything under 256 KB — which is essentially every LibOS object — goes to
Caladan's `smalloc`, which is backed by slabs, which are backed by
`page_alloc`, which is backed by the reserved large-page pool. Only allocations
*above* 256 KB reach glibc's heap.

**2. A pipe costs 64 KB of LibOS memory.**

```c
inline constexpr size_t kPipeSize = 16 * kPageSize;    // limits.h
auto pipe = std::make_shared<StreamPipe>(kPipeSize);   // fs/pipe.cc
std::vector<std::byte> buf_;                           // base/byte_channel.h
```

64 KB is under `kMaxAllocSize`, so it comes from the pool. ~16000 concurrent
pipes is the whole 1 GB reservation, and a guest can open them with nothing more
exotic than `setrlimit(RLIMIT_NOFILE)`.

Past the pool, `lgpage_create()` mmaps into whichever address space the
allocating core is bound to. The pages then go back to smalloc's free lists,
which are **shared LibOS state**, so they are handed out to LibOS code running
under a different address space.

## Ordering

1. `fork()` first, so the child's address space is cloned while the pool is
   intact;
2. the parent opens 17000 pipes — the new large pages are mapped into the
   parent's address space only;
3. the parent closes them, returning the buffers to smalloc;
4. the child opens pipes, is handed that memory, and Junction writes a pipe
   buffer into pages the child's address space has never had.

Measured RSS confirms the cost: 100 pipes -> 16.7 MB, 3000 pipes -> 204.7 MB.
64.8 KB per pipe.

## What this corrects

Earlier analysis in this series ranked the *glibc heap* as the main
guest-drivable path and the large-page pool as unreachable. Both were wrong, for
the same reason: `new_override.cc` had not been accounted for.

- **glibc's heap is nearly unreachable from a guest.** It only serves
  allocations over 256 KB. 300 000 one-page `mmap`s, 400 000 `mprotect` VMA
  splits and 3000 pipes each produced *zero* heap growth — the trace recorded no
  `junction-libc` events in any of them, and `memfd:junction_libos` stayed at
  67.6 MB throughout. It is still a real hazard (it crashes 3/3 when forced, see
  `scripts/heap_overflow_test.sh`) but a guest cannot drive it.
- **The large-page pool is the guest-drivable one.** An earlier attempt to
  overflow it used threads, whose allocations were absorbed by slabs already
  populated at init, and concluded it was unreachable post-fork. The right
  workload is anything that makes Junction allocate *objects* in bulk. Pipes are
  the densest at 64 KB each; sockets, files and epoll watches would all work
  more slowly.

The lesson worth keeping: "which allocator backs this?" is not answerable from
the call site. `std::vector<std::byte>` looks like glibc and is not.

## What fixes it

Nothing local. Raising `LGPAGE_POOL_ENTRIES` moves the cliff; a guest just opens
more pipes. The fix is that a LibOS mapping made after an address space is
cloned must reach that address space — `docs/fork-implementation-plan.md`,
Phases 1-3. When it lands, this test should pass with the pool at its default
size *and* with it shrunk, which is the real assertion: the reservation becomes
an optimisation rather than a correctness requirement.
