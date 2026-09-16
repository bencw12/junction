// memprobe.cc - can a LibOS allocation escape a reserved arena slot?
//
// The plan in docs/libos-memory-plan.md puts all LibOS memory in one memfd-
// backed slot, so that propagating it to another address space is a MAP_FIXED
// of the same file offset. That only works if nothing the LibOS does can place
// memory outside the slot. This runs a battery of allocation shapes from a
// genuine LibOS context and reports where each one landed.
//
// Enable with --debug_libos_escape. Diagnostic only; nothing here runs
// otherwise.

extern "C" {
#include <dlfcn.h>
#include <sys/mman.h>

#include <base/thread.h>
#include <runtime/preempt.h>
}

#include <cstdlib>
#include <cstring>

#include "junction/base/arch.h"
#include "junction/bindings/log.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/memprobe.h"
#include "junction/limits.h"

namespace junction {

namespace {

// Where an arena slot would go: the next free 512 GB slot above Caladan's
// relocated stack (0x520000000000) and large-page (0x510000000000) pools.
constexpr uintptr_t kArenaBase = 0x530000000000UL;
constexpr size_t kArenaLen = 1UL << 39;

bool InArena(uintptr_t a) {
  return a >= kArenaBase && a < kArenaBase + kArenaLen;
}

void Report(const char *what, uintptr_t addr, const char *note = nullptr) {
  if (!addr || addr > ~0xfffUL) {
    LOG(INFO) << "escape:   " << what << ": failed";
    return;
  }
  LOG(INFO) << "escape:   " << what << " -> 0x" << std::hex << addr << std::dec
            << (InArena(addr) ? "  inside the arena slot"
                              : "  OUTSIDE the arena slot")
            << (note ? note : "");
}

}  // namespace

void RunLibOSEscapeProbe() {
  LOG(INFO) << "escape: nominal arena slot 0x" << std::hex << kArenaBase << "-0x"
            << (kArenaBase + kArenaLen) << std::dec;

  uint64_t guest_fs = GetFSBase();
  SetFSBase(perthread_read(runtime_fsbase));
  preempt_disable();

  // 1. An ordinary large malloc. Goes to glibc's mmap path, which the seccomp
  //    filter traps, so a redirect at the trap could place it.
  void *p = std::malloc(64UL << 20);
  Report("malloc(64 MB)", reinterpret_cast<uintptr_t>(p),
         "  [trapped: a redirect could place this]");

  // 2. An aligned allocation. glibc over-allocates and munmaps the tails, so
  //    the placement is glibc's, not ours -- and the trimming munmaps have to
  //    propagate too.
  void *ap = nullptr;
  if (posix_memalign(&ap, 2UL << 20, 8UL << 20) != 0) ap = nullptr;
  Report("posix_memalign(2 MB, 8 MB)", reinterpret_cast<uintptr_t>(ap),
         "  [trapped, but glibc trims with munmap]");

  // 3. Junction's own anonymous mapping, the way Junction actually allocates
  //    for itself. This is the interesting one: ksys_mmap's rip is inside the
  //    ksys range, so the seccomp filter *allows* it and it never traps. A
  //    redirect in the SIGSYS handler cannot see this at all.
  intptr_t r = ksys_mmap(nullptr, 1UL << 20, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Report("ksys_mmap(NULL, 1 MB)", static_cast<uintptr_t>(r),
         "  [NOT trapped: bypasses any SIGSYS redirect]");

  // 4. The zpoline shape: an executable LibOS page at a kernel-chosen address.
  //    An RW arena cannot serve this one even if we did catch it.
  intptr_t x = ksys_mmap(nullptr, kPageSize,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Report("ksys_mmap(NULL, exec page)", static_cast<uintptr_t>(x),
         "  [NOT trapped, and PROT_EXEC: an RW arena cannot back it]");

  // 5. A hinted mapping just past the arena. Nothing stops the LibOS asking.
  intptr_t h = ksys_mmap(reinterpret_cast<void *>(kArenaBase + kArenaLen),
                         1UL << 20, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Report("ksys_mmap(hint just past arena, 1 MB)", static_cast<uintptr_t>(h));

  // 6. dlopen: file-backed, executable, and it runs the loader's own mmaps.
  void *so = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
  Report("dlopen(libm.so.6)", reinterpret_cast<uintptr_t>(so),
         "  [handle, not a mapping -- see the trace for its segments]");

  preempt_enable();
  SetFSBase(guest_fs);
}

}  // namespace junction
