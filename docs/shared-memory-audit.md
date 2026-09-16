# Audit: every place a LibOS allocation can go to one address space

The failure is always the same shape — memory mapped while one `mm` is loaded,
then used while another is. This enumerates every path that can do it, how
likely each is to be reached, and what state each is in.

Method: every `mmap` / `mem_map_*` / `memfd` call site in
`lib/caladan/{runtime,base,iokernel}` and `junction/`, resolved to its enclosing
function and classified as init-time or runtime. Then, for the runtime ones,
what the allocation backs and who else can touch it.

## Where every LibOS allocation actually comes from

Asked directly: what backs `class Process`'s fields, the nodes in its `std::map`s,
the file table, and every other piece of kernel state? Measured rather than
inferred, because the answer is not visible at any call site.

`junction/new_override.cc` overrides global `operator new`:

```c
constexpr size_t kMaxAllocSize = (1UL << 18);        // 256 KB
if (unlikely(size > kMaxAllocSize)) return std::malloc(size);
return smalloc(size);
```

and `smalloc` is backed by the **pre-reserved large-page pool**. Confirmed by
watching where residency lands while a guest holds pipes open (each pipe is a
64 KB `std::vector<std::byte>`):

| | 100 pipes | 3000 pipes | delta |
| --- | --- | --- | --- |
| lgpage pool (`0x510000000000`) Rss | 6.9 MB | 190.3 MB | **+183.4 MB** |
| glibc heap (`memfd:junction_libos`) Rss | 0.7 MB | 1.0 MB | +0.3 MB |

2900 x 64 KB = 181 MB, which is the pool delta. So:

| allocation | route | backing | pre-fork? | shared? |
| --- | --- | --- | --- | --- |
| any LibOS object <= 256 KB — `Process` fields, `std::map` nodes, `shared_ptr` control blocks, `VMArea`s, pipe buffers | `smalloc` -> slab -> `page_alloc` | 1 GB pool at `0x510000000000` | yes | yes |
| > 256 KB — e.g. a large `file_array` | `std::malloc` | 64 MB `memfd:junction_libos`, swept at init | yes | yes |
| uthread stacks | stack allocator | 16 GB pool at `0x520000000000` | yes | yes |
| **memfs file contents** | **`ksys_mmap` direct** | **nothing — 256 MB mapped per file** | **no** | n/a |

**Neither general allocator creates a mapping until its region is exhausted**,
which is why a guest opening 40 000 file descriptors produces zero
`junction-libc` events: the `file_array` exceeds 256 KB and does go to glibc,
but glibc serves it from the 64 MB already-swept heap without growing.

### These reservations are workarounds this work added, not pre-existing design

Worth being exact about, because it changes what "just fix memfs" buys. None of
these pools existed before the multi-address-space work. Upstream Caladan
allocated on demand, privately:

```c
/* git show HEAD:runtime/stack.c */
stack_addr = syscall_mmap(base, sizeof(struct stack), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

/* git show HEAD:base/page.c */
pgaddr = mem_map_anom(pgaddr, PGSIZE_2MB, PGSIZE_2MB, numa_node);
```

`git show HEAD:` finds no `STACK_POOL_ENTRIES`, `stack_pool_end`,
`cfg_shared_runtime_mem`, `LGPAGE_POOL_ENTRIES`, `nr_pooled_lgpages` or
`PAGE_BASE_ADDR` anywhere in committed Caladan.

So the pre-reserved shared regions *are* the current mitigation for the
propagation bug — added by this work, bounded by construction, and each with a
demonstrated crash past its cliff. memfs is not an allocator that failed to use
existing infrastructure; it is the one allocator the mitigation was never
extended to.

That reframes the choice:

- **Scoped (memfs only)** removes the trigger ordinary programs hit, and leaves
  three bounded workarounds in place: 16 GB of reserved address space for
  stacks, 1 GB for pages, a 64 MB pre-grown heap, and a crash past each.
- **Full propagation fix** removes the class, and then all three reservations
  can go away — which also returns the 17 GB of address space and undoes the
  density cost of pre-allocating pools sized by guesswork.

So the complete list of ways a new LibOS mapping can appear after a fork is:

1. **memfs, per file created** — no pre-reservation at all;
2. large-page pool overflow (>1 GB of live LibOS objects);
3. stack pool overflow (>16384 concurrent uthreads);
4. glibc heap growth (>64 MB live in allocations over 256 KB).

Only the first needs no scale. The scoped fix is to give memfs the same
treatment the other three already have.

## Complete cross-reference of Caladan mapping sites

Derived by listing every `mmap`/`mem_map_*` site in `lib/caladan` and marking
whether this work touched its file (`git status` against the submodule's HEAD),
rather than from memory.

**Inside the Junction process:**

| site | what | state |
| --- | --- | --- |
| `base/mem.c` `mem_map_anom` | the generic anonymous allocator | **changed** `MAP_PRIVATE` -> `MAP_SHARED` |
| `base/mem.c` `mem_map_file` / `mem_map_shm` | file and SysV shm helpers | already shared by construction |
| `base/page.c` `lgpage_create` | large-page pool | **pre-reserved `MAP_SHARED`**; cliff at 1 GB |
| `base/page.c` `page_init` | the reservation itself | init |
| `runtime/stack.c` `stack_create` | uthread stacks | **pre-reserved `MAP_SHARED`**; cliff at 16384 |
| `runtime/init.c` `runtime_init` | kthread stacks | **`MAP_SHARED` via `pthread_attr_setstack`** |
| `runtime/net/.../mlx5_init_external.c` | directpath memfd + UAR | `MAP_SHARED`, init |
| `base/thread.c` `thread_alloc_perthread` | per-kthread state | untouched file, but routes through `mem_map_anom` -> `MAP_SHARED`; init |
| `base/pci.c` `pci_map_mem_bar` | PCI BAR | untouched; init, directpath only |
| `runtime/ioqueues.c` `iok_shm_alloc` | iokernel region | untouched; maps once (`if (!r->base)`), init |
| `runtime/storage.c` `storage_init` | storage pool | untouched; init |

**In the iokernel** — `control.c` (x3), `directpath/core.c`, `ksched.c`. A
separate process with its own address space; it cannot affect Junction's.

So every Caladan mapping reachable from the Junction process is now one of:
`MAP_SHARED`, init-time (and therefore converted by `ShareLibOSMemory()` before
the first fork), or one of the two pooled allocators. **No omissions on the
Caladan side.**

The four untouched Junction-process sites are all init-time, which is why
leaving them alone was safe — the sweep is the backstop for anything private
that exists before the first clone.

### And the reason memfs escaped all of it

`junction/new_override.cc` — which predates this work and is unchanged — routes
every C++ allocation to one of two places:

```c
if (unlikely(size > kMaxAllocSize)) return std::malloc(size);   // >256 KB
return smalloc(size);                                            // the rest
```

Re-backing those two places covers essentially all LibOS allocation, and that is
why the mitigation worked as broadly as it did. That centralisation is
pre-existing Junction architecture, not something this work introduced.

memfs is the one LibOS allocator that **does not go through `operator new` at
all** — `MemInode::Create` calls `ksys_mmap` directly. It was never inside
either chokepoint, so re-backing the chokepoints did nothing for it.

That is the whole gap, stated precisely: *the mitigation covered the two
allocators `new_override.cc` routes to, and memfs bypasses `new_override.cc`.*

## Answering it mechanically instead of by enumeration

Everything below this point started as a grep for `mmap` call sites, which is a
weak instrument: it depends on having found every site, and it misses `mprotect`
splits and `munmap`s, which diverge across address spaces just as badly. The
first version of the tracer also classified "LibOS memory" as
`addr >= kVirtualAreaMax` — and memfs extents live at `0x380000000000`, *below*
the line, so five traced workloads reported zero LibOS mappings while memfs was
mapping one per file the whole time.

`CheckPropagationHazard()` replaces that with a test that does not depend on
knowing the sites:

> Once more than one address space exists, a mapping whose address is **not
> inside any process's reserved region** is LibOS memory, and it reaches only
> the address space that was loaded when it was made.

Checked against the *global* reservation set (`mem_areas_`), not the calling
process's own map — during exec the new image is loaded before its `MemoryMap`
is installed, so comparing against the caller's map reports every exec as a
hazard. Uses a try-lock, so a contended check reports "guest" rather than
risking a deadlock on the mapping path.

Run over eight ordinary shell workloads it flags **only** memfs extents, and
exactly in the cases that crash:

| workload | hazard | crashes |
| --- | --- | --- |
| `echo > f; ( cat f )` | — | no |
| `( echo > f ); cat f` | `mmap-memfs-extent 0x380000000000` | **yes** |
| `echo \| tee f; cat f` | `mmap-memfs-extent 0x380000000000` | **yes** |
| `echo aaa \| grep aaa` | — | no |
| five sequential execs | — | no |
| `cp src f; cat f` | — | no |
| `x=$(/bin/echo hi)` | — | no |
| `python3 -c subprocess.run(...)` | — | no |

Coverage is complete for Junction-side mapping *creation*: every raw
`ksys_mmap` site (`perf.h`, `memfs.cc`, `linuxfile.cc`) is hooked, and the rest
go through `KernelMMap`/`KernelMProtect`/`KernelMUnmap`, which are hooked too.
It does not cover Caladan's internal allocators except where they overflow into
`on_runtime_map`, and eight workloads is not a proof.

### It is scheduling-dependent, not structural

`cp` and `tee` have the same shape — a forked child creates a memfs file, the
parent reads it — and only `tee` crashes. The trace says why:

```
/bin/cp src /tmp/m1; cat /tmp/m1     extent created in as=0
( echo hi > /tmp/m2 ); cat /tmp/m2   extent created in as=4294967297
```

The extent lands in whichever address space happens to be loaded when
`MemInode::Create` runs, because the core is not necessarily rebound to the
child's address space at that moment. `cp` is not safe, it is lucky. Whether a
given program survives depends on scheduling, which makes this worse to live
with than the deterministic reading suggested: the same script can work in
testing and fail in production.

## What it costs a guest to reach each pool

Both Caladan pools are guest-reachable with ordinary syscalls. The count needed
is just pool size divided by object size, and the object sizes differ by three
orders of magnitude:

| lever | LibOS cost each | count to exhaust its pool | measured |
| --- | --- | --- | --- |
| **pipe** -> large-page pool (1 GB) | 64 KB | ~16 384 | 17 000 pipes -> 35 `lgpage_create`, crash |
| **thread** -> stack pool (16384 stacks) | 1 stack | 16 384 | 20 000 threads -> 3 680 `stack_create` |
| **VMA split** -> large-page pool | 260 B | ~4 000 000 | 400 000 splits -> **0** events |

Pipes are not special; they are simply the densest object a guest can allocate
per syscall. VMAs reach the same pool but cost 250x more syscalls per byte.

**The guest thread ceiling is the guest's own, not Junction's.**
`RUNTIME_MAX_THREADS` is 100 000. An earlier measurement here stopped at 1023
threads and wrongly concluded threads could not reach the stack pool — that was
glibc's default 8 MB pthread stack exhausting the *guest's* address space. With
`pthread_attr_setstacksize(64 KB)` a guest reaches 20 000 threads and overflows
the pool with no LibOS knobs:

```
stack: high-water mark exceeded the reserved pool of 16384 stacks;
       new stacks are visible only in the address space that allocated them
```

So neither pre-allocation is a high bar. Both are a few thousand ordinary
syscalls away, needing only a raised `RLIMIT_NOFILE` or a smaller pthread stack
— both of which a guest sets for itself.

## The thing that makes this hard to reason about

`junction/new_override.cc` overrides global `operator new`:

```c
constexpr size_t kMaxAllocSize = (1UL << 18);        // 256 KB
if (unlikely(size > kMaxAllocSize)) return std::malloc(size);
return smalloc(size);
```

So **which allocator backs an allocation is not visible at the call site.**
`std::vector<std::byte>` in `byte_channel.h` looks like glibc and is not: under
256 KB it goes to Caladan's `smalloc` -> slab -> `page_alloc` -> the reserved
large-page pool. Only allocations *above* 256 KB reach glibc's heap.

Two earlier rankings in this document were wrong because of it, in opposite
directions. Both are corrected below.

## Ranked by likelihood of being hit

| # | path | needs | blast radius | status |
| --- | --- | --- | --- | --- |
| 1 | **Caladan large-page pool** via `smalloc` | a guest opening ~16000 pipes | every LibOS object under 256 KB | **crashes 3/3, pure guest program** |
| 2 | **memfs extent** (`MemInode::Create`) | fork, then create a file, then read it | that file | **crashes 3/3** |
| 3 | **Caladan stack pool** (`stack_create`) | >16384 concurrent uthreads | uthread stacks | **crashes 5/5** |
| 4 | **Junction's glibc heap** | allocations >256 KB after a fork | any such object | **crashes 3/3 forced**; not guest-drivable |
| 5 | `MemoryMap::Create` `PROT_NONE` guard | a fork between two process creations | address-space bookkeeping | divergence only |

**#1 is the one that matters**, and it needs no debug knobs, no LibOS
modification and no unusual configuration — just a guest that opens pipes. Full
write-up in `docs/bug-guest-driven-libos-allocation.md`; regression test
`scripts/pipe_heap_test.sh`.

memfs is still the *easiest* to trigger (a fork, a file, a read, at no scale at
all), but the pipe path is the one that shows a guest can reach LibOS memory
management directly.

## Caladan: the enumeration is complete

Every `mmap` / `mem_map_*` site in `runtime/` and `base/` is inside a function
named `*_init*`, with exactly two exceptions:

| site | function | backs |
| --- | --- | --- |
| `runtime/stack.c:67` | `stack_create` | uthread stacks |
| `base/page.c:122` | `lgpage_create` | *everything else* |

`iok_shm_alloc` looks like a runtime allocator but maps once (`if (!r->base)`)
and every caller is `ioqueues_init`, `storage_init` or `mlx5_init_verbs`;
afterwards it is a bump allocator inside that one region. The two directpath
mappings are in `mlx5_init_ext_late`, reached once from `runtime_init`.

### Why `lgpage_create` is the one that matters

It is not one allocator among several — it is the root of all of them:

```
thread_slab ("runtime_threads", thread_t control blocks) ─┐
mbuf_slab   ("mbufs", network buffers)                    ├─> slab_create
smalloc_slabs (all Caladan small allocations)             ─┘      │
                                                                  v
                                             page_alloc_on_node(PGSIZE_2MB)
smpage_slab (all 4 KB pages) ──> slab ──────────────>  lgpage_alloc_on_node
                                                                  │
                                                                  v
                                                           lgpage_create
```

So overflowing the 1 GB pool does not lose one thing — it puts uthread control
blocks, network buffers, and every small allocation into a single address space
at once. A `thread_t` reachable from only one `mm` is worse than a stack: the
scheduler dereferences it on every switch.

### `lgpage_create` is reachable — the first attempt used the wrong workload

An earlier measurement here concluded it was not. That was wrong, and the reason
is worth recording. The attempt used **threads**: with the pool shrunk to a
single 2 MB page and 1023 concurrent uthreads, `lgpage_create` was entered 4
times and every one landed during init, before any fork. The conclusion drawn —
"Caladan's large-page consumption is entirely front-loaded" — held only for that
workload. Thread creation reuses slabs already populated at init.

The right workload makes Junction allocate *objects* in bulk. Because
`new_override.cc` sends everything under 256 KB to `smalloc`, and `smalloc` is
backed by this pool, **any object a guest can create in quantity works**. Pipes
are the densest at 64 KB each, so 17000 of them overflow the 1 GB pool outright:

```
page: growing the heap past the reserved pool; new memory is visible only in
      the address space that allocated it
fault with preemption disabled: addr=5100444a0000 pid=2 as=4294967297
```

Sockets, open files and epoll watches would do the same, more slowly.

### Both pools had a warning that never printed

`stack_create` and `lgpage_create` each carried a `log_warn_ratelimited()`
saying new memory "will not be visible in address spaces cloned earlier".
Neither ever appeared in output — the stack one stayed silent while the path was
taken 350 times in a single run. Both are now `log_err_ratelimited` — but that was not enough on its own,
because the rate limiter itself was broken: `__last_us` started at 0 while
`microtime()` counts from runtime start, so **the first occurrence was
suppressed for the whole first second**, which is exactly when init runs. 23
call sites were affected, including `net: out of mbufs`. Fixed; see
`docs/bug-ratelimited-logging.md` and `docs/bug-stack-pool-overflow.md`.

## Junction

| site | when | verdict |
| --- | --- | --- |
| `memfs.cc:116` `MemInode::Create` | per file created | **bug**, see below |
| `memfs.cc:130` `MemInode::MMap` | guest `mmap` of a memfs file | guest-directed, per-mm correct |
| `mm.cc` (several) | guest `mmap`/`mprotect` | guest-directed, per-mm correct |
| `linuxfile.cc:105` | guest file mapping | guest-directed, per-mm correct |
| `jif.cc:156,180` `LoadPhdr` | snapshot restore | maps at the *guest's* addresses; per-mm correct |
| `syscall.cc:56` `SyscallInit` | init | before any clone |
| `shim/backend/init.cc:14` `ShimJmpInit` | init | before any clone |
| `zpoline.cc:69` `InitZpoline` | init | before any clone, but `PROT_EXEC` — see below |
| `perf.h:33` | serverless mode only | not on the default path |

### memfs, in detail

`MemInode::Create` maps a fresh 256 MB extent per file, at a monotonically
increasing address, into whichever address space is current. It also runs *two*
allocators — a never-recycled address counter and a recycled offset bitmap — so
the address/offset identity that would make a stale mapping harmless does not
hold. Measured over 40 create/delete cycles of one file: 1 offset, 40 addresses.
Full analysis in `docs/libos-memory-plan.md`.

### Junction's glibc heap

**The crash is real.** With the LibOS allocating 128 MB through its own glibc
while a forked guest's address space is loaded, and another address space then
dereferencing the result:

```
debug: LibOS malloc(128 MB) -> 0x7a86bffdf010 in address space 4294967297
debug: address space 0 looking for it: ABSENT
debug: writing to it from this address space anyway...
Aborting on signal: unhandled segfault while in Junction syscall handler
```

3/3, `scripts/heap_overflow_test.sh` part 1. This is the failure that matters,
because glibc's free lists are *shared* LibOS state: a chunk mmap'd under one
address space is handed out to LibOS code running under another.

**But a guest cannot drive it, at the sizes tried.** That was the ranking
mistake. Attempts, all with the trace watching for `junction-libc` events:

| workload | reserve | heap growth |
| --- | --- | --- |
| 20 000 one-page `mmap`s | 1 MB | 0 |
| 300 000 one-page `mmap`s | 0 | 0 |
| 400 000 `mprotect` VMA splits | 0 | 0 |

Two reasons, both worth recording:

- `JUNCTION_DEBUG_HEAP_RESERVE_MB` does not cap the heap. The reserve is an
  *extra* pre-growth on top of whatever glibc's main arena already `brk`'d
  during init, so setting it to 0 still leaves several MB of free arena.
- A loop of one-page `mmap`s costs **one** VMArea node, not n: consecutive
  `NULL`-hint mappings get adjacent addresses from `FindFreeRange` and Junction
  merges them. `docs/traces/heap_overflow_race.c` uses `mprotect` on alternate
  pages instead, so each protection boundary forces a split that cannot merge
  back — and 200 000 of those still fit in the arena.

So the 64 MB reserve is far more generous, relative to what a guest can drive
through the VMA path, than the ranking first suggested. The hazard is genuine
and the fix still has to cover it — LibOS heap growth is driven by Junction's
own behaviour, not only the guest's — but it is not the near-term crash the
stack pool is.

### `MemoryMap::Create`'s `PROT_NONE` guard

`MemoryMap::Create` reserves a new guest's range with
`KernelMMap(base, len, PROT_NONE, MAP_FIXED_NOREPLACE)` in the current address
space only, so address spaces forked earlier lack the guard while `mem_areas_`
(global) records the range as taken. Divergence rather than a crash: the kernel
places `NULL`-hint mappings near its own `mmap_base` (~`0x7f...`), far from the
guest region below `kVirtualAreaMax`, so nothing is observed to land in the gap.
Worth fixing with the rest, not on its own.

### `InitZpoline`

Maps `PROT_EXEC | PROT_WRITE | PROT_READ` at a kernel-chosen address. Init-time,
so it predates every clone and is not a bug — but it is the standing example of
an executable LibOS mapping, which is the case an RW arena could not serve. (An
`MFD_EXEC` memfd can, which memfs already uses; see the plan.)

## What is already covered and needs nothing

- **iokernel shm, SysV segments, hugetlbfs pools, directpath** — all
  `MAP_SHARED` or file-backed and established before the first guest, so
  `clone()` inherits them coherently.
- **The startup sweep** — `ShareLibOSMemory()` converts private-writable LibOS
  mappings to shared before the first guest and `AuditLibOSMemory()` checks it.
  At steady state only two private-writable LibOS mappings remain, both
  deliberate.
