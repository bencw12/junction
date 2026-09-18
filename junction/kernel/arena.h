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
// *repairable*: put the memory in a region backed by a single file, so that a
// fault anywhere in it can be turned back into the correct mapping.
//
// Two kinds of region do that here.
//
// A *managed slot* is backed 1:1 by a file: offset = addr - base, and a fault is
// repaired with arithmetic alone, no bookkeeping consulted. memfs uses one.
//
// The *LibOS arena* is a managed slot whose bindings are not 1:1: a shadow map
// records, for every mapped range, the memfd offset and protection behind it,
// and a fault is repaired by looking the range up. Losing the arithmetic
// shortcut is what lets a range be freed and its offset punched without ever
// being reused for something else -- and that quarantine is the correctness
// condition the whole design rests on:
//
//     an address range is never reused for a different (fd, offset) binding
//     until it has been unmapped from every address space that materialised it
//
// With it, every divergence between address spaces is an *absence*, absences
// fault, and faults are repaired here. Without it a repair would silently hand
// back somebody else's bytes. Reuse is deferred to garbage collection, which
// is how the condition is discharged; until then a freed range stays
// quarantined and a fault in it is a genuine use-after-free, reported as one.

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

// Repairs a fault at @addr if it lands in a managed slot and the mapping that
// belongs there permits @required_prot (PROT_READ/WRITE/EXEC of the faulting
// access). Returns false if the address is not managed or the access would
// still fault after repair, in which case the fault is somebody else's problem.
//
// Safe to call with preemption disabled and from signal context: no libc, no
// allocation, and the only lock it may take is never held across a fault.
[[nodiscard]] bool RepairManagedFault(uintptr_t addr, int required_prot);

// Whether @addr lies in a managed slot. For diagnostics and assertions.
[[nodiscard]] bool AddressIsManaged(uintptr_t addr);

// How many faults have been repaired, for tests and for noticing when a
// workload is faulting far more than it should.
[[nodiscard]] uint64_t ManagedFaultCount();

// --- The LibOS arena (Phase 2 of docs/fork-implementation-plan.md) ---

// Creates the arena: one sealed memfd, a PROT_NONE reservation over its slot
// in the virtual address space, and the node pool the shadow map lives in.
// Must run before the first address space is cloned, so that all three exist
// in every address space.
Status<void> InitLibOSArena();

// Maps @len bytes (page-aligned) of fresh arena memory with @prot into the
// calling address space and records the binding. Other address spaces receive
// it on first touch. The memory is zero-filled.
Status<void *> ArenaMap(size_t len, int prot);

// Frees [addr, addr+len), which must be live arena memory: the physical pages
// are punched out of the memfd, the range is unmapped here, and the address
// range and its offsets are quarantined -- never handed out again by ArenaMap
// -- until garbage collection has unmapped them everywhere.
Status<void> ArenaUnmap(void *addr, size_t len);

// Changes the protection of [addr, addr+len), which must be live arena memory,
// here and in the shadow map. An address space that already holds the range
// with the old protection keeps it until an access there faults.
Status<void> ArenaProtect(void *addr, size_t len, int prot);

// Whether @addr is inside the arena's slot at all, and whether it is currently
// live (mapped, not quarantined). Both for diagnostics and tests.
[[nodiscard]] bool ArenaContains(uintptr_t addr);
[[nodiscard]] bool ArenaIsLive(uintptr_t addr);

struct ArenaStats {
  size_t mapped_bytes;       // live
  size_t quarantined_bytes;  // freed, awaiting GC
  size_t live_entries;       // shadow-map entries, live
  size_t dead_entries;       // shadow-map entries, quarantined
  size_t nodes_in_use;       // of the node pool
  size_t nodes_capacity;
  size_t punch_failures;     // frees whose PUNCH_HOLE failed (memory not reclaimed)
};
[[nodiscard]] ArenaStats GetArenaStats();

// Self-test, enabled by --debug_arena_probe: after the first fork, maps arena
// memory in this address space and reads it back from every other one, so the
// shadow-map repair path is exercised on demand rather than waiting for a
// workload to reach it. Also checks that a freed range is not reused. Logs
// "arena probe: PASS" or "arena probe: FAIL: <why>".
void ArenaProbe();

}  // namespace junction
