// arena.cc - see arena.h

extern "C" {
#include <base/log.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
}

#include <algorithm>
#include <atomic>
#include <cstring>

#include "junction/base/arch.h"
#include "junction/base/bits.h"
#include "junction/base/interval_set.h"
#include "junction/bindings/log.h"
#include "junction/bindings/sync.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/as.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/mm.h"

#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#endif
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif

namespace junction {

namespace {

// ---------------------------------------------------------------------------
// Managed slots: the registry the fault handler consults first.
// ---------------------------------------------------------------------------

struct ManagedSlot {
  uintptr_t base;
  size_t len;
  size_t granule;
  int fd;
  int prot;
  const char *name;
  bool shadow;  // bindings come from the arena's shadow map, not arithmetic
};

// Slots are registered during initialization and never removed, so the fault
// path can read them without a lock. There will be a handful: memfs and the
// LibOS arena.
constexpr size_t kMaxSlots = 8;
ManagedSlot slots[kMaxSlots];
std::atomic_size_t nr_slots{0};
std::atomic_uint64_t repairs{0};

const ManagedSlot *FindSlot(uintptr_t addr) {
  size_t n = nr_slots.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; i++) {
    const ManagedSlot &s = slots[i];
    if (addr >= s.base && addr < s.base + s.len) return &s;
  }
  return nullptr;
}

Status<void> RegisterSlot(const char *name, uintptr_t base, size_t len,
                          size_t granule, int fd, int prot, bool shadow) {
  if (base < kVirtualAreaMax) return MakeError(EINVAL);
  if (!granule || (base % granule) || (len % granule)) return MakeError(EINVAL);

  size_t n = nr_slots.load(std::memory_order_relaxed);
  if (n == kMaxSlots) return MakeError(ENOSPC);
  slots[n] = ManagedSlot{base, len, granule, fd, prot, name, shadow};
  nr_slots.store(n + 1, std::memory_order_release);

  LOG(INFO) << "arena: " << name << " manages 0x" << std::hex << base << "-0x"
            << (base + len) << std::dec << " (" << (len >> 30) << " GB, "
            << (shadow ? "shadow map" : "1:1") << ") backed by fd " << fd;
  return {};
}

// ---------------------------------------------------------------------------
// The shadow map's node pool.
//
// The fault handler consults the shadow map, so nothing the map touches may
// itself fault. Its nodes therefore come from one fixed region, mapped
// MAP_SHARED and populated before the first clone, which every address space
// then inherits. This is the single pre-reserved pool the design keeps: it is
// sized from a bound (map nodes are all one size, and a lookup never
// allocates) rather than guessed, and it is what lets everything else be
// mapped on demand.
//
// Allocation and release happen only under the arena lock, from the map
// mutations in ArenaMap/ArenaUnmap/ArenaProtect, so the pool needs no lock of
// its own. The fault path only reads the map.
// ---------------------------------------------------------------------------

constexpr size_t kNodeSize = 128;             // a map node is ~96 bytes
constexpr size_t kNodePoolBytes = 8UL << 20;  // 65536 nodes; ~100 live today
constexpr size_t kNodeCapacity = kNodePoolBytes / kNodeSize;

struct NodePool {
  char *base{nullptr};
  size_t bump{0};
  void *free_list{nullptr};
  size_t in_use{0};
  bool warned{false};
} node_pool;

void *PoolAlloc(size_t sz) {
  BUG_ON(sz > kNodeSize);
  node_pool.in_use++;
  if (node_pool.free_list) {
    void *p = node_pool.free_list;
    node_pool.free_list = *static_cast<void **>(p);
    return p;
  }
  // The pool is the one thing here that cannot grow: it has to exist in every
  // address space before any of them is cloned. Say so before it matters.
  if (unlikely(!node_pool.warned && node_pool.bump > kNodePoolBytes / 2)) {
    node_pool.warned = true;
    log_warn("arena: shadow-map node pool is half full (%zu of %zu nodes)",
             node_pool.in_use, kNodeCapacity);
  }
  if (unlikely(node_pool.bump + kNodeSize > kNodePoolBytes)) {
    log_err("arena: shadow-map node pool exhausted (%zu nodes)", kNodeCapacity);
    BUG_ON(true);
  }
  void *p = node_pool.base + node_pool.bump;
  node_pool.bump += kNodeSize;
  return p;
}

void PoolFree(void *p) {
  *static_cast<void **>(p) = node_pool.free_list;
  node_pool.free_list = p;
  node_pool.in_use--;
}

template <typename T>
struct NodeAllocator {
  using value_type = T;
  NodeAllocator() = default;
  template <typename U>
  NodeAllocator(const NodeAllocator<U> &) noexcept {}
  T *allocate(size_t n) {
    BUG_ON(n != 1);
    return static_cast<T *>(PoolAlloc(sizeof(T)));
  }
  void deallocate(T *p, size_t) noexcept { PoolFree(p); }
  template <typename U>
  bool operator==(const NodeAllocator<U> &) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const NodeAllocator<U> &) const noexcept {
    return false;
  }
};

// ---------------------------------------------------------------------------
// The shadow map and the arena.
// ---------------------------------------------------------------------------

// One mapped range of the arena: where it is, what backs it, how it is
// protected, and whether it has been freed. Two entries merge only if they are
// contiguous in *both* spaces with the same protection and the same state, so
// a merged entry is always one thing a single mmap can reproduce.
struct ShadowEntry {
  [[nodiscard]] uintptr_t get_start() const { return start_; }
  [[nodiscard]] uintptr_t get_end() const { return end_; }

  void TrimTail(uintptr_t new_end) {
    assert(end_ > new_end);
    end_ = new_end;
  }

  void TrimHead(uintptr_t new_start) {
    assert(start_ < new_start);
    offset_ += new_start - start_;
    start_ = new_start;
  }

  bool TryMergeRight(const ShadowEntry &lhs) {
    if (start_ != lhs.end_ || prot_ != lhs.prot_ || dead_ != lhs.dead_)
      return false;
    if (lhs.offset_ + (lhs.end_ - lhs.start_) != offset_) return false;
    start_ = lhs.start_;
    offset_ = lhs.offset_;
    return true;
  }

  uintptr_t start_;
  uintptr_t end_;
  uint64_t offset_;  // into the memfd
  int prot_;
  bool dead_;  // freed and quarantined: never reused, never repaired
};

template <typename T>
using PoolSet = ExclusiveIntervalSet<T, NodeAllocator<std::pair<const uintptr_t, T>>>;

struct Arena {
  rt::Spin lock;
  bool ready{false};
  uintptr_t base{0};
  size_t len{0};
  int fd{-1};

  // Two spaces, tracked separately rather than one derived from the other, so
  // that a later mremap can keep a range contiguous in VA while fragmenting it
  // in the memfd (docs/fork-implementation-plan.md, "Growth"). Both hold live
  // and quarantined ranges alike: FindFreeRange skips both, which is the
  // quarantine.
  PoolSet<ShadowEntry> va;
  PoolSet<SimpleInterval> off;

  size_t mapped{0};
  size_t quarantined{0};
  size_t punch_failures{0};
} arena;

// The arena's slot: 512 GB at 0x530000000000, the next free 512 GB-aligned
// slot above Caladan's relocated page pool (0x510000000000) and stack pool
// (0x520000000000), both of which this is going to replace. 512 GB alignment
// so the whole thing sits in one PGD entry, which keeps page-table sharing
// possible later (docs/shared-page-tables.md).
constexpr uintptr_t kArenaPreferredBase = 0x530000000000;
constexpr size_t kArenaSize = 512UL << 30;
constexpr size_t kSlotAlign = 512UL << 30;

bool RangeIsLive(uintptr_t start, uintptr_t end) {
  // Every byte of [start, end) must be inside a live entry. Entries are
  // exclusive and merged where possible, so walking by end is enough.
  uintptr_t cur = start;
  while (cur < end) {
    Status<std::reference_wrapper<ShadowEntry>> f = arena.va.Find(cur);
    if (!f) return false;
    const ShadowEntry &e = f->get();
    if (e.dead_) return false;
    cur = e.end_;
  }
  return true;
}

bool ArenaRepair(uintptr_t addr, int required_prot) {
  uintptr_t start, end;
  uint64_t off;
  int prot;
  {
    // Preemption is disabled for the critical section. That is what makes
    // this safe from the fault handler: the only other holder is a mutator
    // that cannot fault while holding it (its nodes are in the pool), and it
    // cannot be descheduled while holding it either.
    rt::SpinGuard g(arena.lock);
    Status<std::reference_wrapper<ShadowEntry>> f = arena.va.Find(addr);
    if (!f) return false;
    const ShadowEntry &e = f->get();
    // A freed range is quarantined: this is a use-after-free in the LibOS,
    // and repairing it would hide exactly that.
    if (e.dead_) return false;
    // The mapping that belongs here would still fault for this access. That
    // is a genuine protection fault, not an absence.
    if ((e.prot_ & required_prot) != required_prot) return false;
    start = e.start_;
    end = e.end_;
    off = e.offset_;
    prot = e.prot_;
  }

  // The whole entry, not just the faulting page: entries are one contiguous
  // (fd, offset) binding, so one mmap reproduces the lot. Outside the lock,
  // because a syscall has no business under a spinlock. If the entry was
  // freed between the lookup and here, this maps a punched, quarantined range
  // that reads as zeros and is never handed out again -- harmless.
  intptr_t ret = ksys_mmap(reinterpret_cast<void *>(start), end - start, prot,
                           MAP_SHARED | MAP_FIXED, arena.fd, off);
  if (unlikely(ret < 0)) return false;
  repairs.fetch_add(1, std::memory_order_relaxed);
  log_info_once("arena: repaired a fault in the LibOS arena at 0x%lx", addr);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Managed slots, public surface.
// ---------------------------------------------------------------------------

Status<void> RegisterManagedSlot(const char *name, uintptr_t base, size_t len,
                                 size_t granule, int fd, int prot) {
  return RegisterSlot(name, base, len, granule, fd, prot, false);
}

bool AddressIsManaged(uintptr_t addr) { return FindSlot(addr) != nullptr; }

uint64_t ManagedFaultCount() {
  return repairs.load(std::memory_order_relaxed);
}

bool RepairManagedFault(uintptr_t addr, int required_prot) {
  const ManagedSlot *s = FindSlot(addr);
  if (!s) return false;
  if (s->shadow) return ArenaRepair(addr, required_prot);

  // A 1:1 slot. Round down to the granule the slot was populated in.
  // Repairing a single page would be correct -- the binding is 1:1 at every
  // offset -- but a 256 MB memfs extent would then take 65536 faults to walk.
  //
  // Note that this repairs any address in the slot, including one whose
  // granule is not currently allocated to anything. Consulting the owner's
  // bookkeeping would mean taking its lock in signal context, and the failure
  // it would catch -- LibOS code dereferencing a wild pointer that happens to
  // land in a 4 TiB region above kVirtualAreaMax -- is not one worth a lock
  // on this path. Guests cannot reach here at all: they are confined below
  // kVirtualAreaMax.
  if ((s->prot & required_prot) != required_prot) return false;
  uintptr_t start = addr & ~(s->granule - 1);
  off_t off = static_cast<off_t>(start - s->base);

  // ksys_mmap, not KernelMMapFixed: this runs in signal context, where the
  // tracing wrappers and anything else that touches libc are not welcome.
  intptr_t ret = ksys_mmap(reinterpret_cast<void *>(start), s->granule,
                           s->prot, MAP_SHARED | MAP_FIXED, s->fd, off);
  if (unlikely(ret < 0)) return false;

  // MAP_FIXED replaces atomically and the binding is identical, so two cores
  // repairing the same granule is harmless -- the loser installs the same
  // mapping the winner did.
  repairs.fetch_add(1, std::memory_order_relaxed);

  // Caladan's logger, not LOG(): this runs in signal context, where libc's
  // locks belong to whatever was interrupted. Once per slot, so it says the
  // mechanism worked without turning a hot path into a log.
  log_info_once("arena: repaired a fault in %s at 0x%lx (offset 0x%lx)",
                s->name, addr, static_cast<unsigned long>(off));
  return true;
}

// ---------------------------------------------------------------------------
// The LibOS arena, public surface.
// ---------------------------------------------------------------------------

Status<void> InitLibOSArena() {
  // 2a. The memfd. Sealed against resizing: the kernel enforces the size at
  // fault time, not at mmap time, so a size that could change is not a bound.
  // MFD_EXEC where the kernel has it, so code can live here later.
  int fd = memfd_create("junction-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING |
                                              MFD_EXEC);
  if (fd < 0)
    fd = memfd_create("junction-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return MakeError(errno);
  if (ftruncate(fd, kArenaSize) < 0) return MakeError(errno);
  if (fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
    return MakeError(errno);

  // The slot: one PROT_NONE reservation, made before the first clone so that
  // it exists in every address space. It proves the region is free, keeps
  // anything else from being placed inside it, and means a fault in it is
  // always a missing *range*, never a missing region. ArenaMap installs real
  // mappings over it with MAP_FIXED, which is the only way anything is ever
  // placed here.
  constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
  void *res = mmap(reinterpret_cast<void *>(kArenaPreferredBase), kArenaSize,
                   PROT_NONE, kFlags | MAP_FIXED_NOREPLACE, -1, 0);
  if (res == reinterpret_cast<void *>(kArenaPreferredBase)) {
    arena.base = kArenaPreferredBase;
  } else {
    if (res != MAP_FAILED) munmap(res, kArenaSize);
    res = mmap(nullptr, kArenaSize + kSlotAlign, PROT_NONE, kFlags, -1, 0);
    if (res == MAP_FAILED) {
      LOG(ERR) << "arena: could not reserve " << (kArenaSize >> 30)
               << " GB for the LibOS arena";
      return MakeError(ENOMEM);
    }
    arena.base = AlignUp(reinterpret_cast<uintptr_t>(res), kSlotAlign);
    LOG(INFO) << "arena: 0x" << std::hex << kArenaPreferredBase
              << " was taken; reserved 0x" << arena.base << std::dec
              << " instead";
  }
  if (arena.base < kVirtualAreaMax) {
    LOG(ERR) << "arena: the reserved region landed at 0x" << std::hex
             << arena.base << ", below kVirtualAreaMax" << std::dec;
    return MakeError(ENOMEM);
  }
  arena.len = kArenaSize;
  arena.fd = fd;

  // 2b. The node pool. MAP_SHARED so every address space sees the same
  // nodes; MAP_POPULATE so consulting the map never takes even a minor fault.
  void *pool = mmap(nullptr, kNodePoolBytes, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (pool == MAP_FAILED) return MakeError(ENOMEM);
  node_pool.base = static_cast<char *>(pool);

  arena.ready = true;
  return RegisterSlot("libos-arena", arena.base, arena.len, kPageSize, fd,
                      PROT_READ | PROT_WRITE | PROT_EXEC, true);
}

Status<void *> ArenaMap(size_t len, int prot) {
  if (unlikely(!arena.ready)) return MakeError(ENODEV);
  if (unlikely(!len || len % kPageSize)) return MakeError(EINVAL);

  uintptr_t va;
  uint64_t off;
  intptr_t ret;
  {
    rt::SpinGuard g(arena.lock);
    Status<uintptr_t> v =
        arena.va.FindFreeRange(0, len, arena.base + arena.len, arena.base);
    if (!v) return MakeError(ENOMEM);
    Status<uintptr_t> o = arena.off.FindFreeRange(0, len, arena.len, 0);
    if (!o) return MakeError(ENOMEM);
    va = *v;
    off = *o;

    // Under the lock, so two mappers cannot be handed the same range. A
    // single syscall; the hold is microseconds.
    ret = ksys_mmap(reinterpret_cast<void *>(va), len, prot,
                    MAP_SHARED | MAP_FIXED, arena.fd, off);
    if (likely(ret >= 0)) {
      arena.va.Insert(ShadowEntry{va, va + len, off, prot, false});
      arena.off.Insert(SimpleInterval{off, off + len});
      arena.mapped += len;
    }
  }

  if (unlikely(ret < 0)) {
    // This is MAP_FIXED over our own reservation. If the kernel refuses it,
    // something has gone badly wrong with the slot, and it should be loud.
    LOG(ERR) << "arena: MAP_FIXED of " << len << " bytes at 0x" << std::hex
             << va << std::dec << " refused: " << Error(-ret);
    return MakeError(static_cast<int>(-ret));
  }
  return reinterpret_cast<void *>(va);
}

Status<void> ArenaUnmap(void *addr, size_t len) {
  if (unlikely(!arena.ready)) return MakeError(ENODEV);
  uintptr_t start = reinterpret_cast<uintptr_t>(addr), end = start + len;
  if (unlikely(!len || start % kPageSize || len % kPageSize))
    return MakeError(EINVAL);
  if (unlikely(start < arena.base || end > arena.base + arena.len))
    return MakeError(EINVAL);

  size_t failed = 0;
  {
    rt::SpinGuard g(arena.lock);
    if (!RangeIsLive(start, end)) return MakeError(EINVAL);

    // Free the physical pages. munmap alone would not: on a shared memfd
    // mapping neither munmap nor MADV_DONTNEED returns a page, only a hole
    // punch does (docs/traces/memfd_reclaim.c). Piece by piece, because the
    // range may span several entries with different offsets.
    uintptr_t cur = start;
    while (cur < end) {
      const ShadowEntry &e = arena.va.Find(cur)->get();
      uintptr_t piece_end = std::min(e.end_, end);
      uint64_t off = e.offset_ + (cur - e.start_);
      long ret = ksyscall(SYS_fallocate, arena.fd,
                          FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off,
                          piece_end - cur);
      if (unlikely(ret < 0)) failed++;
      cur = piece_end;
    }

    // Quarantine: the entries stay, marked dead, so the range and its offsets
    // are never handed out again. Garbage collection is what will eventually
    // remove them, after unmapping the range from every address space.
    arena.va.Modify(
        start, end, [](const ShadowEntry &e) { return !e.dead_; },
        [](ShadowEntry &e) { e.dead_ = true; });

    // Put the reservation back over the range, rather than munmap: a hole
    // would be free address space that a foreign mmap(NULL) could land in,
    // and the slot is meant to stay ours. Same single syscall either way.
    // Other address spaces keep their stale mapping over the punched offsets
    // -- it reads as zeros and the range is quarantined, so it cannot be
    // confused with anything live -- until garbage collection.
    ksys_mmap(addr, len, PROT_NONE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    arena.mapped -= len;
    arena.quarantined += len;
    arena.punch_failures += failed;
  }

  if (unlikely(failed))
    LOG(WARN) << "arena: " << failed << " hole punch(es) failed freeing 0x"
              << std::hex << start << "-0x" << end << std::dec
              << "; the range is quarantined but its memory is not reclaimed";
  return {};
}

Status<void> ArenaProtect(void *addr, size_t len, int prot) {
  if (unlikely(!arena.ready)) return MakeError(ENODEV);
  uintptr_t start = reinterpret_cast<uintptr_t>(addr), end = start + len;
  if (unlikely(!len || start % kPageSize || len % kPageSize))
    return MakeError(EINVAL);
  if (unlikely(start < arena.base || end > arena.base + arena.len))
    return MakeError(EINVAL);

  rt::SpinGuard g(arena.lock);
  if (!RangeIsLive(start, end)) return MakeError(EINVAL);
  int ret = ksys_mprotect(addr, len, prot);
  if (unlikely(ret < 0)) return MakeError(-ret);
  arena.va.Modify(
      start, end,
      [prot](const ShadowEntry &e) { return !e.dead_ && e.prot_ != prot; },
      [prot](ShadowEntry &e) { e.prot_ = prot; });
  return {};
}

bool ArenaContains(uintptr_t addr) {
  return arena.ready && addr >= arena.base && addr < arena.base + arena.len;
}

bool ArenaIsLive(uintptr_t addr) {
  if (!ArenaContains(addr)) return false;
  rt::SpinGuard g(arena.lock);
  Status<std::reference_wrapper<ShadowEntry>> f = arena.va.Find(addr);
  return f && !f->get().dead_;
}

ArenaStats GetArenaStats() {
  ArenaStats st{};
  if (!arena.ready) return st;
  rt::SpinGuard g(arena.lock);
  st.mapped_bytes = arena.mapped;
  st.quarantined_bytes = arena.quarantined;
  for (const auto &[end, e] : arena.va) {
    if (e.dead_)
      st.dead_entries++;
    else
      st.live_entries++;
  }
  st.nodes_in_use = node_pool.in_use;
  st.nodes_capacity = kNodeCapacity;
  st.punch_failures = arena.punch_failures;
  return st;
}

// ---------------------------------------------------------------------------
// The probe.
// ---------------------------------------------------------------------------

namespace {

struct ProbeRange {
  void *p;
  size_t len;
  unsigned char byte;
};

struct ProbeVisit {
  const ProbeRange *ranges;
  size_t n;
  size_t mismatches;
  size_t visited;
  size_t present_before;  // a range was already readable there: test invalid
  size_t absent_after;    // a range was not readable after its repair
  size_t map_unreadable;  // /proc could not be read; nothing was verified
};

// Runs in another address space with preemption disabled: no LOG, no
// allocation. Before touching anything it checks, from that address space's
// own /proc map, that each range is genuinely absent -- covered only by the
// PROT_NONE reservation -- so the faults below are known to be real rather
// than inferred from a count. Each first touch then faults, and the fault is
// repaired through the preemption-disabled path in the signal handler -- the
// same one the LibOS's own allocators take under their spinlocks. Afterwards
// the map is read again: the repair must have installed the range.
void ProbeReadBack(uint64_t, void *ctx) {
  ProbeVisit &v = *static_cast<ProbeVisit *>(ctx);
  v.visited++;
  for (size_t i = 0; i < v.n; i++) {
    uintptr_t s = reinterpret_cast<uintptr_t>(v.ranges[i].p);
    uintptr_t e = s + v.ranges[i].len;
    int before = RangeReadableHere(s, e);
    if (before < 0) v.map_unreadable++;
    if (before > 0) v.present_before++;

    const volatile unsigned char *b =
        static_cast<const volatile unsigned char *>(v.ranges[i].p);
    if (b[0] != v.ranges[i].byte || b[v.ranges[i].len - 1] != v.ranges[i].byte)
      v.mismatches++;

    if (RangeReadableHere(s, e) != 1) v.absent_after++;
  }
}

}  // namespace

void ArenaProbe() {
  static bool done = false;
  if (done) return;
  done = true;

  if (!arena.ready) {
    LOG(WARN) << "arena probe: FAIL: arena not initialized";
    return;
  }

  // A spread of sizes, so entries of different shapes are exercised: one
  // page, a few pages, a large-page's worth, and one that gets freed.
  constexpr size_t kSizes[] = {4096, 3 * 4096, 2UL << 20, 64UL << 10};
  ProbeRange r[4];
  for (size_t i = 0; i < 4; i++) {
    Status<void *> p = ArenaMap(kSizes[i], PROT_READ | PROT_WRITE);
    if (!p) {
      LOG(WARN) << "arena probe: FAIL: ArenaMap(" << kSizes[i]
                << ") failed: " << p.error();
      return;
    }
    r[i] = ProbeRange{*p, kSizes[i], static_cast<unsigned char>(0x41 + i)};
    unsigned char *b = static_cast<unsigned char *>(*p);
    b[0] = r[i].byte;
    b[kSizes[i] - 1] = r[i].byte;
  }

  // Protection changes must land in the shadow map, so that a repair
  // elsewhere reproduces the current protection rather than the original.
  if (Status<void> ret = ArenaProtect(r[1].p, r[1].len, PROT_READ); !ret) {
    LOG(WARN) << "arena probe: FAIL: ArenaProtect failed: " << ret.error();
    return;
  }

  // Quarantine: a freed range is not reused, and reads as dead afterwards.
  void *freed = r[3].p;
  if (Status<void> ret = ArenaUnmap(r[3].p, r[3].len); !ret) {
    LOG(WARN) << "arena probe: FAIL: ArenaUnmap failed: " << ret.error();
    return;
  }
  if (ArenaIsLive(reinterpret_cast<uintptr_t>(freed))) {
    LOG(WARN) << "arena probe: FAIL: freed range still reported live";
    return;
  }
  Status<void *> again = ArenaMap(r[3].len, PROT_READ | PROT_WRITE);
  if (!again) {
    LOG(WARN) << "arena probe: FAIL: ArenaMap after free failed: "
              << again.error();
    return;
  }
  if (*again == freed) {
    LOG(WARN) << "arena probe: FAIL: quarantined range 0x" << std::hex
              << reinterpret_cast<uintptr_t>(freed) << std::dec
              << " was reused";
    return;
  }
  r[3] = ProbeRange{*again, r[3].len, 0x45};
  static_cast<unsigned char *>(*again)[0] = 0x45;
  static_cast<unsigned char *>(*again)[r[3].len - 1] = 0x45;

  // Cross-address-space: every other live address space is missing all four
  // ranges. Reading them there faults and must be repaired from the shadow
  // map -- including the read-only one, whose repair must install PROT_READ.
  uint64_t before = ManagedFaultCount();
  ProbeVisit visit{r, 4, 0, 0, 0, 0, 0};
  size_t visited = ForEachOtherAddressSpace(ProbeReadBack, &visit);
  uint64_t repaired = ManagedFaultCount() - before;

  ArenaStats st = GetArenaStats();
  if (visited == 0) {
    LOG(WARN) << "arena probe: FAIL: no other address space to visit";
    return;
  }
  if (visit.map_unreadable) {
    LOG(WARN) << "arena probe: FAIL: could not read /proc maps in "
              << visit.map_unreadable << " visit(s); absence not verified";
    return;
  }
  if (visit.present_before) {
    LOG(WARN) << "arena probe: FAIL: " << visit.present_before
              << " range(s) were already mapped in another address space "
                 "before being touched -- the faults would not have been real";
    return;
  }
  if (visit.absent_after) {
    LOG(WARN) << "arena probe: FAIL: " << visit.absent_after
              << " range(s) not readable after their repair";
    return;
  }
  if (visit.mismatches) {
    LOG(WARN) << "arena probe: FAIL: " << visit.mismatches
              << " byte mismatch(es) reading arena memory from " << visited
              << " other address space(s)";
    return;
  }
  // Each range is one entry, and each is absent from every other address
  // space, so every visit repairs every range once.
  if (repaired < 4 * visited) {
    LOG(WARN) << "arena probe: FAIL: expected at least " << 4 * visited
              << " repairs, saw " << repaired;
    return;
  }
  LOG(INFO) << "arena probe: PASS: 4 range(s) absent in each other address "
               "space before touch, present after; "
            << repaired << " fault(s) repaired across " << visited
            << " other address space(s); " << st.live_entries
            << " live, " << st.dead_entries << " quarantined, "
            << st.nodes_in_use << "/" << st.nodes_capacity << " nodes";
}

}  // namespace junction
