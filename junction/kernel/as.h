// as.h - multiple address spaces for guest processes
//
// Junction normally runs every guest process inside the single address space of
// the host process, giving each one a private slice of the virtual address
// range. That makes fork() impossible: a forked child must see its parent's
// memory at the parent's addresses, and two guests cannot occupy the same
// addresses in one address space.
//
// This file adds real address spaces. Dune gave a library OS direct ownership
// of its page tables so it could do exactly this kind of thing; we keep the
// same division of labour but let Linux remain the page-table implementation,
// which is what makes the resulting fork() semantics identical to Linux's --
// copy-on-write, reverse mapping, reclaim and accounting are all the kernel's
// own.
//
// A new address space is created by cloning the host process: Linux performs
// the copy-on-write duplication itself. The clone is a task that never runs; a
// small kernel module (kern/junction_as.c) takes a reference to its mm and
// hands back a handle, after which the clone is discarded. Scheduling kthreads
// then bind themselves to a handle to run that guest's threads.
//
// Two invariants make this work, and both are checked at runtime:
//
//   1. Every guest's memory region that does not belong to the process being
//      forked is excluded from the clone (MADV_DONTFORK), so one guest's
//      address space never contains another's memory.
//
//   2. All of the LibOS's own writable memory is MAP_SHARED, so it is the same
//      memory in every address space rather than a copy. This is not merely
//      about keeping data structures coherent: a kthread keeps running on its
//      own stack across a switch, so a private stack would lose every frame
//      pushed in the other address space. AuditLibOSMemory() reports any
//      violation.

#pragma once

extern "C" {
#include <base/thread.h>
}

#include <cstdint>
#include <string>
#include <vector>

#include "junction/base/error.h"
#include "junction/bindings/sync.h"

namespace junction {

// The address space Junction started in. Always valid, never released.
inline constexpr uint64_t kRootAddressSpace = 0;

// A half-open virtual address range.
struct AddressRange {
  uintptr_t start;
  uintptr_t end;
  [[nodiscard]] size_t Length() const { return end - start; }
};

// Initializes address-space support. Converts the LibOS's private writable
// mappings to shared ones and opens the address-space device. Returns an error
// if the device is unavailable, in which case Junction keeps running in a
// single address space and fork() reports ENOSYS.
Status<void> InitAddressSpaces();

// Whether guest processes can be given their own address spaces.
[[nodiscard]] bool MultiAddressSpaceEnabled();

// Creates a copy-on-write snapshot of the calling thread's current address
// space, omitting every range in @exclude. Returns a handle.
//
// The caller must hold the fork lock (see AddressSpaceForkLock) so that the
// exclusions cannot be changed by a concurrent clone.
Status<uint64_t> CloneCurrentAddressSpace(
    const std::vector<AddressRange> &exclude);

// Drops a handle. The address space is freed once no kthread is bound to it.
void ReleaseAddressSpace(uint64_t handle);

// Binds the running kthread to @handle. Cheap and idempotent: a kthread that is
// already bound to @handle does nothing.
void ActivateAddressSpace(uint64_t handle);

// Like ActivateAddressSpace, but returns false instead of killing the process
// when the address space no longer exists. For cleanup paths that may run
// after the address space has been released.
[[nodiscard]] bool TryActivateAddressSpace(uint64_t handle);

// What the kernel says the running kthread is bound to, as opposed to what
// this core last recorded. Diagnostics only: it is a system call.
[[nodiscard]] uint64_t QueryBoundAddressSpace();

// The handle the running kthread is bound to.
[[nodiscard]] uint64_t GetActiveAddressSpace();

// True once a second address space has been created, i.e. from the first
// clone onwards. Before that every mapping is trivially visible everywhere.
[[nodiscard]] bool MultipleAddressSpacesExist();

// Serializes address-space cloning. Held across the exclusion changes and the
// clone itself, because MADV_DONTFORK is a property of the address space and
// two clones would otherwise race over it.
[[nodiscard]] rt::Mutex &AddressSpaceForkLock();

// Reports LibOS mappings that are still private and writable, which would
// diverge instead of being shared once an address space is cloned. Returns the
// number of offending mappings; logs each one.
size_t AuditLibOSMemory(bool log_details = true);

// Checks that every live address space agrees with the caller's on the LibOS's
// mappings, by visiting each one and diffing /proc/thread-self/maps. Reports
// mappings that are absent, unexpectedly present, or differently protected;
// returns the total.
//
// AuditLibOSMemory() answers "could this diverge?" before the first clone.
// This answers "has it diverged?" afterwards, and is the net for whatever the
// static reasoning missed. Enabled by --debug_as_audit, which runs it after
// every fork.
//
// Must be called on a runtime stack: it binds the calling kthread to other
// address spaces, where the guest's own stack does not exist.
size_t AuditAddressSpaceCoherence(bool log_details = true);

// Calls @fn bound to each live address space other than the caller's, with
// preemption disabled for the duration of each call (see the note in
// AuditAddressSpaceCoherence on why). @fn must not block, allocate or log.
// Returns the number of address spaces visited. Same stack requirement as the
// audit.
size_t ForEachOtherAddressSpace(void (*fn)(uint64_t handle, void *ctx),
                                void *ctx);

// In the calling kthread's *current* address space, whether every byte of
// [start, end) is covered by a readable mapping: 1 if so, 0 if not (a gap, or
// only the PROT_NONE reservation), -1 if the map could not be read. For tests
// that must know a range is genuinely absent before touching it, rather than
// inferring it from a fault count. Raw syscalls only, so safe with preemption
// disabled; uses the audit's scratch, so it may only be called from inside a
// ForEachOtherAddressSpace() visit, which holds that scratch's lock.
[[nodiscard]] int RangeReadableHere(uintptr_t start, uintptr_t end);

// Logs every mapping of the current address space that overlaps [start, end).
// Diagnostic for MM Panic: says what is really there.
void DumpMappingsOverlapping(uintptr_t start, uintptr_t end);

// Unmaps @stray from the address space @handle, which was just cloned.
//
// MADV_DONTFORK keeps other guests' memory out of a clone only as far as it
// was marked, and two things slip past the marking. Guests that share the
// address space being cloned -- vfork+exec'd children live in their parent's
// -- keep running: one that mmap()s inside its own region after the marking
// pass makes a mapping with no mark on it, and one that execs after the list
// of regions was drawn up reserves a region that is not on it. Either ends up
// in the clone. The owner later unmaps its region from its own address space
// only; the region is handed out again; and the first exec given it inside
// the clone finds it occupied ("mm: Create wanted ... got 0 ... err=17", then
// "MM Panic"). That took make -j56 in one of sixteen concurrent sandboxes to
// hit, about one such build in seven.
//
// So the clone is swept afterwards, with a list drawn up after the clone: a
// region is registered before anything is mapped in it, so that list covers
// everything that can have been copied, and unmapping a range that turns out
// to be empty costs nothing.
void SweepClonedAddressSpace(uint64_t handle,
                             const std::vector<AddressRange> &stray);

}  // namespace junction
