// arena.h - LibOS memory that is repaired lazily instead of propagated eagerly
//
// Junction creates a guest address space by cloning the host process, so a
// mapping made after the clone exists only in the address space that made it.
// Everything the LibOS maps later is therefore invisible to every guest that
// already exists -- and the guest finds out by segfaulting on an address that
// looks perfectly valid from wherever you are debugging.
//
// The fix is not to propagate mappings eagerly, which would mean touching
// every address space on every allocation. It is to make the divergence
// *repairable*: put the memory in a region that is backed 1:1 by a single
// file, so that a fault anywhere in it can be turned back into the correct
// mapping with arithmetic alone.
//
//     addr in [slot.base, slot.base + slot.len)  =>  offset = addr - slot.base
//
// A managed slot is such a region. When a fault lands in one, the handler maps
// the enclosing granule from the slot's file at the matching offset and
// retries the instruction. No bookkeeping is consulted, no lock is taken, and
// the repair is idempotent, so two cores faulting on the same range race
// harmlessly.
//
// The correctness condition is the one the whole design rests on:
//
//     an address range is never reused for a different (fd, offset) binding
//     until it has been unmapped from every address space that materialised it
//
// With it, every divergence between address spaces is an *absence*, absences
// fault, and faults are repaired here. Without it a repair would silently hand
// back somebody else's bytes. See UnmapFromAllAddressSpaces(), which is how
// memfs discharges that condition when a file is deleted.

#pragma once

#include <cstddef>
#include <cstdint>

#include "junction/base/error.h"

namespace junction {

// Registers [base, base+len) as backed 1:1 by @fd, so that faults in it are
// repaired by mapping @granule bytes at the matching offset with @prot.
//
// @base, @len and @granule must all be granule-aligned, and @base must lie
// above kVirtualAreaMax: a slot inside the guest-addressable region would make
// a guest's own fault look repairable.
Status<void> RegisterManagedSlot(const char *name, uintptr_t base, size_t len,
                                 size_t granule, int fd, int prot);

// Repairs a fault at @addr if it lands in a managed slot. Returns false if the
// address is not managed, in which case the fault is somebody else's problem.
//
// Runs in signal context on the syscall stack: no libc, no allocation, no
// locks.
[[nodiscard]] bool RepairManagedFault(uintptr_t addr);

// Whether @addr lies in a managed slot. For diagnostics and assertions.
[[nodiscard]] bool AddressIsManaged(uintptr_t addr);

// How many faults have been repaired, for tests and for noticing when a
// workload is faulting far more than it should.
[[nodiscard]] uint64_t ManagedFaultCount();

}  // namespace junction
