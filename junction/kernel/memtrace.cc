// memtrace.cc - see memtrace.h

extern "C" {
#include <fcntl.h>
#include <sys/mman.h>

#include <base/thread.h>
}

#include <string>

#include "junction/base/time.h"
#include "junction/bindings/log.h"
#include "junction/kernel/as.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/memtrace.h"
#include "junction/kernel/mm.h"
#include "junction/kernel/proc.h"
#include "junction/limits.h"

namespace junction {

namespace {

int trace_fd = -1;

// Manual formatters. snprintf() would be far more pleasant and would also call
// into the libc this file exists to observe.
char *PutStr(char *p, char *end, const char *s) {
  while (*s && p < end) *p++ = *s++;
  return p;
}

char *PutHex(char *p, char *end, uint64_t v) {
  static const char kDigits[] = "0123456789abcdef";
  char tmp[16];
  int n = 0;
  if (!v) tmp[n++] = '0';
  while (v && n < 16) {
    tmp[n++] = kDigits[v & 0xf];
    v >>= 4;
  }
  while (n && p < end) *p++ = tmp[--n];
  return p;
}

char *PutDec(char *p, char *end, int64_t v) {
  char tmp[24];
  int n = 0;
  bool neg = v < 0;
  uint64_t u = neg ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
  if (!u) tmp[n++] = '0';
  while (u && n < 24) {
    tmp[n++] = static_cast<char>('0' + (u % 10));
    u /= 10;
  }
  if (neg && p < end) *p++ = '-';
  while (n && p < end) *p++ = tmp[--n];
  return p;
}

const char *SourceName(MemTraceSource s) {
  switch (s) {
    case MemTraceSource::kJunctionInternal: return "junction";
    case MemTraceSource::kJunctionLibc: return "junction-libc";
    case MemTraceSource::kCaladan: return "caladan";
    case MemTraceSource::kGuestSyscall: return "guest";
  }
  return "?";
}

// Where the address lands, which is independent of who asked for it. A guest
// mapping outside the guest region -- or a LibOS mapping inside one -- is
// exactly the confusion this trace exists to find.
const char *RegionName(uintptr_t addr) {
  if (!addr) return "none";
  if (addr < kVirtualAreaMax) return "guest-range";
  return "libos-range";
}

}  // namespace

bool MemTraceEnabled() { return trace_fd >= 0; }

Status<void> InitMemTrace(std::string_view path) {
  if (path.empty()) return {};

  std::string p(path);
  int fd = ksys_open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  if (fd < 0) return MakeError(-fd);
  trace_fd = fd;

  static const char kHeader[] =
      "# junction LibOS memory trace\n"
      "# us op source region addr len prot flags fd off caller pid tid as "
      "in_kernel preempt result\n";
  ksys_write(trace_fd, kHeader, sizeof(kHeader) - 1);
  LOG(INFO) << "memtrace: writing to " << p;
  return {};
}

void MemTraceRecord(const MemTraceEvent &ev) {
  if (likely(trace_fd < 0)) return;

  // Junction-side context. Guarded, because this can run on a Caladan thread
  // with no Junction thread attached.
  pid_t pid = 0, tid = 0;
  int in_kernel = -1;
  if (IsJunctionThread()) {
    Thread &th = mythread();
    tid = th.get_tid();
    pid = th.get_process().get_pid();
    in_kernel = th.in_kernel() ? 1 : 0;
  }

  char buf[512];
  char *p = buf;
  char *end = buf + sizeof(buf) - 2;

  p = PutDec(p, end, Time::Now().Microseconds());
  p = PutStr(p, end, " ");
  p = PutStr(p, end, ev.op);
  p = PutStr(p, end, " ");
  p = PutStr(p, end, SourceName(ev.src));
  p = PutStr(p, end, " ");
  p = PutStr(p, end, RegionName(ev.addr ? ev.addr
                                        : static_cast<uintptr_t>(ev.result)));
  p = PutStr(p, end, " 0x");
  p = PutHex(p, end, ev.addr);
  p = PutStr(p, end, " ");
  p = PutDec(p, end, static_cast<int64_t>(ev.len));
  p = PutStr(p, end, " 0x");
  p = PutHex(p, end, static_cast<uint64_t>(ev.prot));
  p = PutStr(p, end, " 0x");
  p = PutHex(p, end, static_cast<uint64_t>(ev.flags));
  p = PutStr(p, end, " ");
  p = PutDec(p, end, ev.fd);
  p = PutStr(p, end, " ");
  p = PutDec(p, end, ev.off);
  p = PutStr(p, end, " 0x");
  p = PutHex(p, end, ev.caller);
  p = PutStr(p, end, " ");
  p = PutDec(p, end, pid);
  p = PutStr(p, end, " ");
  p = PutDec(p, end, tid);
  p = PutStr(p, end, " ");
  p = PutDec(p, end, static_cast<int64_t>(GetActiveAddressSpace()));
  p = PutStr(p, end, " ");
  p = PutDec(p, end, in_kernel);
  p = PutStr(p, end, " ");
  p = PutDec(p, end, preempt_enabled() ? 1 : 0);
  p = PutStr(p, end, " 0x");
  p = PutHex(p, end, static_cast<uint64_t>(ev.result));
  *p++ = '\n';

  // O_APPEND plus a single write keeps lines from interleaving across kthreads.
  ksys_write(trace_fd, buf, p - buf);
}

void MemTraceMap(MemTraceSource src, const char *op, uintptr_t addr, size_t len,
                 int prot, int flags, int fd, int64_t off, uintptr_t caller,
                 int64_t result) {
  MemTraceEvent ev = {op, src, addr, len, prot, flags, fd, off, caller, result};
  MemTraceRecord(ev);
  CheckPropagationHazard(op, addr ? addr : static_cast<uintptr_t>(result), len);
}

// Is this mapping a propagation hazard?
//
// The test is mechanical, and deliberately does not depend on knowing which
// code path made the mapping: a mapping inside the calling process's own
// reservation is that guest's memory and belongs in one address space, and
// anything else is the LibOS's and has to be visible in all of them. Once a
// second address space exists, any LibOS mapping made from here on reaches
// only the address space that was loaded at the time.
//
// Classifying by address range instead (>= kVirtualAreaMax) is what hid the
// memfs bug: extents used to live at 0x380000000000, below the line, so they
// were counted as guest memory across five traced workloads. Phase 1 moved
// them above it, which is why CheckFrozenViolation() can now use the cheap
// address test -- but the ownership test is kept here, because this check runs
// only under --trace_libos_mem and can afford it, and because it stays correct
// if some future region is ever placed below the line again.
void CheckPropagationHazard(const char *op, uintptr_t addr, size_t len) {
  if (likely(!MultipleAddressSpacesExist())) return;
  if (!addr || addr > ~0xfffUL) return;
  // Against the *global* reservation set, not the calling process's own map:
  // during exec the new image is loaded before its MemoryMap is installed, so
  // comparing against the caller's map reports every exec as a hazard.
  if (MemoryMap::IsInSomeReservation(addr)) return;
  if (!IsJunctionThread()) return;
  Thread &th = mythread();

  static std::atomic_int reported{0};
  if (reported.fetch_add(1, std::memory_order_relaxed) >= 32) return;
  LOG(WARN) << "PROPAGATION HAZARD: " << op << " 0x" << std::hex << addr << "-0x"
            << (addr + len) << std::dec << " is outside pid "
            << th.get_process().get_pid() << "'s reservation, made in address "
            << "space " << GetActiveAddressSpace()
            << " -- other address spaces will not see it";
}

// Is this a structural change to memory that other address spaces already have?
//
// CheckPropagationHazard() above catches the *absence* case: a LibOS mapping
// made after a clone, which the other address spaces never receive. This
// catches the opposite and more dangerous one -- a mapping they *did* inherit,
// changed in only one of them. An absence faults, and a fault can be repaired
// lazily; a stale live mapping cannot, because nothing ever traps on it.
//
// Which operations qualify, and why these four:
//
//   munmap    removes the VMA here only. The other address spaces keep a live
//             VMA over the same pages, so the moment the range is reused they
//             are looking at a different object at the same address.
//   mprotect  changes permissions here only. Hardening that applies in one
//             address space out of N is not hardening.
//   mremap    moves the VMA here only; the others keep the old address.
//   madvise(MADV_DONTNEED)
//             on private memory this discards the contents outright, and on
//             shared memory it means something else entirely (the page
//             survives and reads back its old bytes). Both are traps -- see
//             docs/traces/memfd_reclaim.c. MADV_REMOVE is deliberately not
//             flagged: punching a hole in the shared object propagates through
//             i_mmap to every address space, which is what we want freeing to
//             do.
//
// The test is the same mechanical one as CheckPropagationHazard: a range
// inside some guest's reservation is that guest's memory and is supposed to
// change in one address space only. Anything else is the LibOS's.
//
// This is always on, unlike the trace. Before the first clone it is one
// relaxed load; the bug it catches is silent and has cost days already.
void CheckFrozenViolation(const char *op, uintptr_t addr, size_t len) {
  if (likely(!MultipleAddressSpacesExist())) return;
  if (!addr || addr > ~0xfffUL) return;
  // Guest memory, which is supposed to change in one address space only.
  //
  // An address test, not MemoryMap::IsInSomeReservation(). The reservation set
  // is guarded by the *global* MemoryMap spin lock, and this check sits on the
  // unmap path that every exiting process walks, so consulting it put a global
  // lock acquire -- and the preemption point in releasing it -- into the
  // teardown hot path. That is worth avoiding on its own merits.
  //
  // It is *not* a fix for the intermittent fork-storm hang documented in
  // docs/bug-fork-storm-hang.md. Removing the lock was measured against
  // keeping it and the hang rate did not move; an earlier arm that suggested
  // otherwise was 0 hangs in 10 runs, which at the observed ~22% rate happens
  // 8% of the time by chance.
  //
  // The address test is only correct because of Phase 1. memfs extents used to
  // live at 0x380000000000, below the line, so classifying by address counted
  // them as guest memory and hid the one bug reachable without scale. They are
  // now above kVirtualAreaMax with every other LibOS region, which is what
  // makes the cheap test sound again.
  if (addr < kVirtualAreaMax) return;

  static std::atomic_int reported{0};
  int n = reported.fetch_add(1, std::memory_order_relaxed);
  if (n >= 32) return;

  // May run on a Caladan thread with no Junction thread attached -- the
  // runtime's own allocators reach here, and they are a real source of this.
  pid_t pid = IsJunctionThread() ? mythread().get_process().get_pid() : -1;
  LOG(WARN) << "FROZEN VIOLATION: " << op << " 0x" << std::hex << addr << "-0x"
            << (addr + len) << std::dec << " is LibOS memory (pid " << pid
            << ", address space " << GetActiveAddressSpace()
            << ") -- other address spaces keep the old mapping"
            << (n == 31 ? " [further reports suppressed]" : "");
}

void FrozenViolationProbe() {
  alignas(4096) static char page[4096];
  page[0] = 1;  // fault it in, so the mprotect has something to act on

  // Drop write permission, and never write to it again. In this address space
  // the page is now read-only; in every other one it is still writable, which
  // is a real divergence that AuditAddressSpaceCoherence() can see -- and a
  // structural change to shared LibOS memory, which is what
  // CheckFrozenViolation() reports. One probe exercises both.
  Status<void> ret = KernelMProtect(page, sizeof(page), PROT_READ);
  if (!ret) LOG(WARN) << "frozen probe: mprotect failed: " << ret.error();
}

// Caladan's weak hook: the runtime telling us it mapped memory for itself.
extern "C" void on_runtime_map(const char *what, void *addr, size_t len,
                               int prot, int flags) {
  if (likely(!MemTraceEnabled())) return;
  MemTraceMap(MemTraceSource::kCaladan, what, reinterpret_cast<uintptr_t>(addr),
              len, prot, flags, -1, 0, 0, reinterpret_cast<int64_t>(addr));
}

}  // namespace junction
