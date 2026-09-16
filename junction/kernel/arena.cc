// arena.cc - see arena.h

extern "C" {
#include <base/log.h>
#include <sys/mman.h>
}

#include <atomic>

#include "junction/bindings/log.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/mm.h"

namespace junction {

namespace {

struct ManagedSlot {
  uintptr_t base;
  size_t len;
  size_t granule;
  int fd;
  int prot;
  const char *name;
};

// Slots are registered during initialization and never removed, so the fault
// path can read them without a lock. There will be a handful: memfs today, the
// LibOS arena next.
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

}  // namespace

Status<void> RegisterManagedSlot(const char *name, uintptr_t base, size_t len,
                                 size_t granule, int fd, int prot) {
  if (base < kVirtualAreaMax) return MakeError(EINVAL);
  if (!granule || (base % granule) || (len % granule)) return MakeError(EINVAL);

  size_t n = nr_slots.load(std::memory_order_relaxed);
  if (n == kMaxSlots) return MakeError(ENOSPC);
  slots[n] = ManagedSlot{base, len, granule, fd, prot, name};
  nr_slots.store(n + 1, std::memory_order_release);

  LOG(INFO) << "arena: " << name << " manages 0x" << std::hex << base << "-0x"
            << (base + len) << std::dec << " (" << (len >> 30)
            << " GB, granule " << (granule >> 20) << " MB) backed by fd " << fd;
  return {};
}

bool AddressIsManaged(uintptr_t addr) { return FindSlot(addr) != nullptr; }

uint64_t ManagedFaultCount() {
  return repairs.load(std::memory_order_relaxed);
}

bool RepairManagedFault(uintptr_t addr) {
  const ManagedSlot *s = FindSlot(addr);
  if (!s) return false;

  // Round down to the granule the slot was populated in. Repairing a single
  // page would be correct -- the binding is 1:1 at every offset -- but a 256 MB
  // memfs extent would then take 65536 faults to walk.
  //
  // Note that this repairs any address in the slot, including one whose granule
  // is not currently allocated to anything. Consulting the owner's bookkeeping
  // would mean taking its lock in signal context, and the failure it would
  // catch -- LibOS code dereferencing a wild pointer that happens to land in a
  // 4 TiB region above kVirtualAreaMax -- is not one worth a lock on this path.
  // Guests cannot reach here at all: they are confined below kVirtualAreaMax.
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

}  // namespace junction
