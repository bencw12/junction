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

// Returns the pages of [addr, addr+len), which must be live arena memory, to
// the kernel while keeping the mapping and the binding: the range reads as
// zeros afterwards, here and everywhere. What MADV_DONTNEED means on private
// anonymous memory, which on a shared memfd mapping it does not do.
Status<void> ArenaDiscard(void *addr, size_t len);

// mremap for arena memory: [old, old+old_len) must be live. Shrinking frees
// the tail. Growing extends in place when the addresses after the range are
// free, else -- with MREMAP_MAYMOVE -- moves the range to a fresh address:
// the bytes never move, only the virtual address does, because the content
// lives at a memfd offset that the new address is bound to. The old address
// is quarantined. MREMAP_FIXED is not supported. Returns the new address.
Status<void *> ArenaRemap(void *old, size_t old_len, size_t new_len,
                          int flags);

// The LibOS's own memory syscalls, trapped by seccomp from Junction's glibc.
// Serves the ones the arena can (anonymous private mmap, and munmap /
// mprotect / mremap / madvise of arena memory) and stores the syscall's result
// in *res; returns false for the rest, which the caller executes natively.
[[nodiscard]] bool RouteLibOSMemSyscall(long sysn, long a0, long a1, long a2,
                                        long a3, long a4, long a5, long *res);

// Whether @addr is inside the arena's slot at all, and whether it is currently
// live (mapped, not quarantined). Both for diagnostics and tests.
[[nodiscard]] bool ArenaContains(uintptr_t addr);
[[nodiscard]] bool ArenaIsLive(uintptr_t addr);

// --- Garbage collection (Phase 5): retiring quarantined ranges ---
//
// A freed range cannot be reused until every address space that materialised
// it has dropped its mapping, and nothing tells those address spaces on its
// own. So freeing appends a tombstone to a log, every address space carries a
// cursor into that log, an address space applies the tombstones past its
// cursor when it next enters the kernel (batch-limited, in the mm that needs
// it, while it is loaded), and a range leaves quarantine when the smallest
// cursor over all live address spaces has passed it. An address space that
// never enters the kernel pins the epoch; under pressure the collector visits
// it and applies on its behalf.
//
// The property that makes this forgiving: applying a tombstone means putting
// the reservation back over the range, and applying one that no longer needs
// applying -- or applying the same one twice -- only creates an absence that
// the fault handler repairs. So a cursor need only ever be *at most* the
// truth, never exact.

// The cursor slot for an address space, created on first use. A MemoryMap
// caches the pointer so the per-syscall check is two loads. @inherited seeds
// a new slot: a clone starts where its parent was *before* the clone.
[[nodiscard]] uint64_t *ArenaGcSlot(uint64_t as_handle, uint64_t inherited);
// The cursor of an address space's slot, 0 if it has none. For seeding a
// clone: read *before* the clone, so the child starts at or behind the truth.
[[nodiscard]] uint64_t ArenaGcCursorOf(uint64_t as_handle);
// Drops the slot when the address space is released, so it stops pinning.
void ArenaGcForget(uint64_t as_handle);

// Does the address space behind @cursor have tombstones to apply?
[[nodiscard]] bool ArenaGcPending(const uint64_t *cursor);
// Applies one batch of pending tombstones to the *current* address space and
// advances *cursor. The caller must be bound to that address space.
void ArenaGcApply(uint64_t *cursor);

// One collection: releases every tombstone all live address spaces have
// applied; under pressure, or when @force, visits the stragglers first.
void ArenaGcCollect(bool force);
// Starts the collector thread.
void StartArenaGc();

struct ArenaStats {
  size_t mapped_bytes;       // live
  size_t quarantined_bytes;  // freed, awaiting GC
  size_t released_bytes;     // left quarantine so far
  size_t tombstones_pending; // in the log, not yet released
  size_t gc_walks;           // pressure-triggered straggler visits
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
