// memtrace.h - a trace of every memory mapping operation Junction performs
//
// Junction's LibOS and its guests share one host process, so every mmap,
// munmap, mprotect and madvise the process issues is either the LibOS acting
// for itself or the LibOS acting on behalf of a guest. Which one it is decides
// whether the mapping has to be visible from every guest address space, and
// getting that wrong is silent until a guest faults on an address that looks
// perfectly valid from wherever you are debugging.
//
// This records each operation with enough context to attribute it after the
// fact, rather than relying on a heuristic at the point of the call. Enable it
// with --trace_libos_mem <path>.
//
// The tracer cannot use libc. It exists partly to observe Junction's *own*
// glibc allocating, and a malloc inside the tracer would recurse into the thing
// being traced. Everything here formats into a stack buffer and writes with a
// raw system call.

#pragma once

#include <cstddef>
#include <cstdint>

#include <string_view>

#include "junction/base/error.h"
#include "junction/kernel/ksys.h"  // for MemTraceSource

namespace junction {

struct MemTraceEvent {
  const char *op;      // "mmap", "munmap", "mprotect", "madvise", ...
  MemTraceSource src;
  uintptr_t addr;
  size_t len;
  int prot;
  int flags;
  int fd;              // -1 when anonymous
  int64_t off;
  uintptr_t caller;    // return address, or the trapping rip
  int64_t result;      // what the operation returned
};

// Opens the trace. Safe to call when tracing is disabled; everything below
// then compiles down to a predictable-branch no-op.
Status<void> InitMemTrace(std::string_view path);

[[nodiscard]] bool MemTraceEnabled();

// Records one event. Never allocates, never calls into libc.
void MemTraceRecord(const MemTraceEvent &ev);

// Flags a mapping that only reaches the address space it was made in.
void CheckPropagationHazard(const char *op, uintptr_t addr, size_t len);

// Test-only. Deliberately performs one structural change to LibOS memory, so
// that CheckFrozenViolation() can be regression tested against a violation
// that is guaranteed to exist. Enabled by --debug_frozen_probe.
//
// A guardrail with no test rots: every real violation it knows about has now
// been fixed, so without this the check would be asserted only by the absence
// of false positives. The probe mprotects one page of the LibOS's own .bss to
// the permissions it already has -- structurally a change in one address
// space, semantically nothing at all.
void FrozenViolationProbe();

// Convenience for the common shapes.
void MemTraceMap(MemTraceSource src, const char *op, uintptr_t addr, size_t len,
                 int prot, int flags, int fd, int64_t off, uintptr_t caller,
                 int64_t result);

}  // namespace junction
