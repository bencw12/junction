// as.cc - multiple address spaces for guest processes (see as.h)

extern "C" {
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <base/thread.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>

#include "junction/base/arch.h"
#include "junction/bindings/log.h"
#include "junction/bindings/sync.h"
#include "junction/junction.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/as.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/mm.h"
#include "junction/limits.h"
#include "kern/junction_as.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef __WCLONE
#define __WCLONE 0x80000000
#endif

namespace junction {

namespace {

// The device that lets a kthread bind itself to another address space.
int as_dev_fd = -1;

// Stack handed to the throwaway clone that materializes a new address space.
// Deliberately MAP_PRIVATE: each clone gets its own copy-on-write page, so a
// signal delivered to one of them cannot disturb Junction or its siblings.
// The stack the parked clone runs on for the instant between clone() and its
// SIGKILL. MAP_PRIVATE on purpose, and exempt from the sharing audit: nothing
// shared ever lives here -- the clone never returns and touches nothing
// Junction owns -- and making it shared would put a stack that two clones
// could run on concurrently into every address space.
void *clone_stack_top = nullptr;
constexpr size_t kCloneStackSize = 16 * kPageSize;

rt::Mutex fork_lock;

/* ------------------------------------------------------------------ */
/* Live address space registry                                         */
/* ------------------------------------------------------------------ */

// The coherence audit has to visit every address space, and nothing else in
// Junction keeps a list: a handle lives in the MemoryMap that owns it, and
// walking every process to collect them would need locks this path cannot
// safely take. A flat array is enough -- it is only ever appended to, scanned,
// and compacted on release.
//
// In .bss, so the startup sweep makes it shared like every other piece of
// LibOS state. A private registry would give each address space its own idea
// of which address spaces exist.
constexpr size_t kMaxTrackedAS = 1024;
struct {
  rt::Spin lock;
  uint64_t handles[kMaxTrackedAS];
  size_t n{0};
  bool overflowed{false};
} as_registry;

void RegisterAddressSpace(uint64_t handle) {
  rt::SpinGuard g(as_registry.lock);
  if (as_registry.n == kMaxTrackedAS) {
    if (!as_registry.overflowed) {
      as_registry.overflowed = true;
      LOG(WARN) << "as: more than " << kMaxTrackedAS
                << " address spaces; the coherence audit will be partial";
    }
    return;
  }
  as_registry.handles[as_registry.n++] = handle;
}

void UnregisterAddressSpace(uint64_t handle) {
  rt::SpinGuard g(as_registry.lock);
  for (size_t i = 0; i < as_registry.n; i++) {
    if (as_registry.handles[i] != handle) continue;
    as_registry.handles[i] = as_registry.handles[--as_registry.n];
    return;
  }
}

// Which address space each kthread is currently bound to. Kept in Caladan's
// per-kthread area (addressed through %gs) rather than in thread-local storage,
// because %fs is repointed at the guest's TLS while guest code runs.
DEFINE_PERTHREAD(uint64_t, active_as);

/* ------------------------------------------------------------------ */
/* /proc/self/maps                                                     */
/* ------------------------------------------------------------------ */

struct Mapping {
  uintptr_t start;
  uintptr_t end;
  int prot;
  bool shared;
  bool writable;
  char tag[32];  // "[stack]", "[heap]", "[vdso]", ... or empty
};

size_t ParseHex(const char *&p, uintptr_t &out) {
  uintptr_t v = 0;
  size_t n = 0;
  for (;; p++, n++) {
    char c = *p;
    if (c >= '0' && c <= '9')
      v = (v << 4) | static_cast<uintptr_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      v = (v << 4) | static_cast<uintptr_t>(c - 'a' + 10);
    else
      break;
  }
  out = v;
  return n;
}

// Reads the calling thread's memory map into @buf.
//
// /proc/thread-self, never /proc/self: the latter resolves to the thread group
// leader, which is parked in the root address space forever. Reading it would
// report the root's mappings no matter which address space the calling kthread
// is bound to -- which is exactly the question this file exists to answer.
//
// openat/pread64/close rather than read(), because those three are on the
// seccomp allowlist and read() is not: a raw read() from LibOS code traps to
// the SIGSYS handler and comes back -ENOSYS. Before the filter is installed
// read() works, which is why this only broke when the audit started running
// after init. seq_file honours the explicit offset, so pread is a faithful
// substitute.
Status<size_t> ReadMaps(char *buf, size_t buflen) {
  int fd = ksys_openat(AT_FDCWD, "/proc/thread-self/maps", O_RDONLY, 0);
  if (fd < 0) return MakeError(-fd);

  size_t off = 0;
  while (off < buflen - 1) {
    ssize_t ret = ksys_pread(fd, buf + off, buflen - 1 - off,
                             static_cast<off_t>(off));
    if (ret < 0) {
      ksys_close(fd);
      return MakeError(static_cast<int>(-ret));
    }
    if (ret == 0) break;
    off += static_cast<size_t>(ret);
  }
  ksys_close(fd);
  buf[off] = '\0';
  return off;
}

// Parses the output of ReadMaps into @out. Returns the number of mappings,
// which may exceed @max if the buffer was too small.
size_t ParseMaps(const char *buf, Mapping *out, size_t max) {
  size_t n = 0;
  const char *p = buf;
  while (*p) {
    Mapping m;
    std::memset(&m, 0, sizeof(m));
    if (ParseHex(p, m.start) == 0) break;
    if (*p++ != '-') break;
    ParseHex(p, m.end);
    if (*p++ != ' ') break;

    m.prot = 0;
    if (p[0] == 'r') m.prot |= PROT_READ;
    if (p[1] == 'w') m.prot |= PROT_WRITE;
    if (p[2] == 'x') m.prot |= PROT_EXEC;
    m.writable = p[1] == 'w';
    m.shared = p[3] == 's';
    p += 4;

    // Skip offset, device and inode; the tag (if any) is last on the line.
    const char *eol = p;
    while (*eol && *eol != '\n') eol++;
    const char *tag = p;
    while (tag < eol && *tag != '[' && !(tag[0] == '/' && tag[-1] == ' '))
      tag++;
    if (tag < eol) {
      size_t len = std::min(static_cast<size_t>(eol - tag), sizeof(m.tag) - 1);
      std::memcpy(m.tag, tag, len);
      m.tag[len] = '\0';
    }

    if (n < max) out[n] = m;
    n++;
    p = *eol ? eol + 1 : eol;
  }
  return n;
}

constexpr size_t kMapsBufSize = 512 * 1024;
constexpr size_t kMaxMappings = 4096;

/* ------------------------------------------------------------------ */
/* Scratch                                                             */
/* ------------------------------------------------------------------ */

// Everything that reads /proc/thread-self/maps shares these buffers.
//
// They are allocated once, from InitAddressSpaces(), and never freed. Both
// properties are load-bearing:
//
//   - allocated before the first clone, so the buffers exist in every address
//     space. The coherence audit reads them while bound to somebody else's
//     address space, and a buffer mmap'd after a clone would be absent there.
//     An audit that faults on its own scratch is worse than no audit;
//   - never freed, because unmapping them would be a frozen violation of
//     exactly the kind CheckFrozenViolation() reports.
//
// The cost is ~750 KB of shared memory, plus another ~230 KB when the
// coherence audit is enabled.
char *maps_text = nullptr;
Mapping *maps_ref = nullptr;    // the reference snapshot
Mapping *maps_other = nullptr;  // one other address space, for the diff

void *AllocSharedScratch(size_t len) {
  void *p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

bool InitScratch(bool with_coherence_audit) {
  maps_text = static_cast<char *>(AllocSharedScratch(kMapsBufSize));
  maps_ref = static_cast<Mapping *>(
      AllocSharedScratch(kMaxMappings * sizeof(Mapping)));
  if (with_coherence_audit)
    maps_other = static_cast<Mapping *>(
        AllocSharedScratch(kMaxMappings * sizeof(Mapping)));
  return maps_text && maps_ref && (!with_coherence_audit || maps_other);
}

// Reads and parses this address space's map into @out. Returns the count.
Status<size_t> SnapshotMaps(Mapping *out) {
  Status<size_t> len = ReadMaps(maps_text, kMapsBufSize);
  if (!len) return MakeError(len);
  return std::min(ParseMaps(maps_text, out, kMaxMappings), kMaxMappings);
}

// A mapping that belongs to the LibOS rather than to a guest process.
// The address space is partitioned: guests are confined below
// kVirtualAreaMax and the LibOS lives above it. Classify by that, not by the
// *current* process's regions -- with several guests alive at once, another
// process's memory (excluded from every clone but its own) would otherwise be
// reported as a LibOS mapping that failed to propagate.
bool IsLibOSMapping(const Mapping &m) { return m.start >= kVirtualAreaMax; }

// Mappings the sweep must leave alone.
bool IsExemptFromSharing(const Mapping &m) {
  if (!m.writable || m.shared) return true;
  // Kernel-owned mappings.
  if (!std::strcmp(m.tag, "[vdso]") || !std::strcmp(m.tag, "[vvar]") ||
      !std::strcmp(m.tag, "[vsyscall]") || !std::strcmp(m.tag, "[vvar_vclock]"))
    return true;
  // The initial thread's stack. Junction's main thread parks in the Caladan
  // scheduler and runs on a runtime stack from then on, so nothing shared
  // lives here -- and replacing it would cost the kernel's automatic stack
  // growth, which is a worse trade than leaving it private.
  if (!std::strcmp(m.tag, "[stack]")) return true;
  return false;
}

/*
 * Replaces one private mapping with a shared mapping holding the same bytes.
 *
 * The contents are pushed into a memfd and the memfd is then mapped back over
 * the same addresses. Everything here uses raw system calls: between the write
 * and the mmap the region's old contents are no longer authoritative, so the
 * code must not touch memory inside it -- and libc would, if only to set errno.
 */
Status<void> ShareOneMapping(const Mapping &m) {
  size_t len = m.end - m.start;

  long fd = ksyscall(__NR_memfd_create, "junction_libos", MFD_CLOEXEC);
  if (fd < 0) return MakeError(-fd);

  long ret = ksyscall(__NR_ftruncate, static_cast<int>(fd), len);
  if (ret < 0) {
    ksyscall(__NR_close, static_cast<int>(fd));
    return MakeError(-ret);
  }

  // Copy the current contents into the file. This reads the region, so it must
  // happen while the region is still mapped.
  size_t off = 0;
  while (off < len) {
    ret = ksyscall(__NR_pwrite64, static_cast<int>(fd),
                   reinterpret_cast<const char *>(m.start) + off, len - off,
                   static_cast<off_t>(off));
    if (ret <= 0) {
      ksyscall(__NR_close, static_cast<int>(fd));
      return MakeError(ret < 0 ? -ret : EIO);
    }
    off += static_cast<size_t>(ret);
  }

  ret = ksyscall(__NR_mmap, reinterpret_cast<void *>(m.start), len, m.prot,
                 MAP_SHARED | MAP_FIXED, static_cast<int>(fd), 0L);
  ksyscall(__NR_close, static_cast<int>(fd));
  if (ret < 0 || static_cast<uintptr_t>(ret) != m.start) return MakeError(EIO);
  return {};
}

}  // namespace

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

rt::Mutex &AddressSpaceForkLock() { return fork_lock; }

bool MultiAddressSpaceEnabled() { return as_dev_fd >= 0; }

uint64_t GetActiveAddressSpace() { return perthread_read(active_as); }

std::atomic_bool multiple_as{false};
bool MultipleAddressSpacesExist() {
  return multiple_as.load(std::memory_order_relaxed);
}

void ActivateAddressSpace(uint64_t handle) {
  if (likely(perthread_read(active_as) == handle)) return;
  if (unlikely(as_dev_fd < 0)) return;
  long ret = ksyscall(__NR_ioctl, as_dev_fd, JUNCTION_AS_SWITCH, handle);
  if (unlikely(ret < 0)) {
    LOG(ERR) << "as: failed to switch to address space " << handle << ": "
             << Error(-ret);
    syscall_exit(-1);
  }
  perthread_store(active_as, handle);
}

bool TryActivateAddressSpace(uint64_t handle) {
  if (likely(perthread_read(active_as) == handle)) return true;
  if (unlikely(as_dev_fd < 0)) return true;
  long ret = ksyscall(__NR_ioctl, as_dev_fd, JUNCTION_AS_SWITCH, handle);
  if (ret < 0) return false;
  perthread_store(active_as, handle);
  return true;
}

void ReleaseAddressSpace(uint64_t handle) {
  if (handle == kRootAddressSpace) return;
  UnregisterAddressSpace(handle);
  ArenaGcForget(handle);  // so a dead address space stops pinning the epoch
  long ret = ksyscall(__NR_ioctl, as_dev_fd, JUNCTION_AS_RELEASE, handle);
  if (unlikely(ret < 0))
    LOG(ERR) << "as: failed to release address space " << handle << ": "
             << Error(-ret);
}

// Defined in ksys.S, where the seccomp filter permits Junction's own system
// calls. Clones the host process onto @child_stack; the clone parks forever
// without touching any memory Junction owns. Returns the clone's pid to the
// caller and never returns in the clone.
extern "C" long junction_as_clone_parked(unsigned long flags, void *child_stack);

Status<uint64_t> CloneCurrentAddressSpace(
    const std::vector<AddressRange> &exclude) {
  assert(fork_lock.IsHeld());
  if (unlikely(!MultiAddressSpaceEnabled())) return MakeError(ENOSYS);

  // The clone inherits this address space's stale arena mappings as of the
  // clone, so it must apply tombstones from where we are *now* -- read before
  // the clone, since another thread may advance our cursor meanwhile (earlier
  // than the truth is safe; later is not). And its slot must exist from the
  // moment the address space does, below, or a collection in the window
  // before its MemoryMap is built would not count it.
  uint64_t inherited_gc_cursor = ArenaGcCursorOf(GetActiveAddressSpace());

  // Keep the excluded ranges out of the clone. MADV_DONTFORK is a per-VMA
  // flag, so this has to be undone afterwards or the ranges would vanish from
  // every future clone as well.
  size_t marked = 0;
  for (const AddressRange &r : exclude) {
    long ret = ksyscall(__NR_madvise, reinterpret_cast<void *>(r.start),
                        r.Length(), MADV_DONTFORK);
    // ENOMEM only means part of the range is unmapped in this address space,
    // which is the common case: the range belongs to another guest.
    if (ret < 0 && ret != -ENOMEM) {
      LOG(ERR) << "as: MADV_DONTFORK failed: " << Error(-ret);
      break;
    }
    marked++;
  }

  auto undo = [&] {
    for (size_t i = 0; i < marked; i++)
      ksyscall(__NR_madvise, reinterpret_cast<void *>(exclude[i].start),
               exclude[i].Length(), MADV_DOFORK);
  };

  if (marked != exclude.size()) {
    undo();
    return MakeError(ENOMEM);
  }

  // Block every signal so the clone starts with them blocked and cannot be
  // interrupted into running a handler on its borrowed stack.
  sigset_t all, saved;
  sigfillset(&all);
  ksyscall(__NR_rt_sigprocmask, SIG_SETMASK, &all, &saved, sizeof(sigset_t));

  long pid = junction_as_clone_parked(
      CLONE_FILES | CLONE_FS | CLONE_SYSVSEM, clone_stack_top);

  ksyscall(__NR_rt_sigprocmask, SIG_SETMASK, &saved,
           static_cast<sigset_t *>(nullptr), sizeof(sigset_t));
  undo();

  if (pid < 0) return MakeError(-pid);

  // Take a reference to the clone's address space, then discard the clone.
  junction_as_adopt arg;
  std::memset(&arg, 0, sizeof(arg));
  arg.pid = static_cast<unsigned int>(pid);
  long ret = ksyscall(__NR_ioctl, as_dev_fd, JUNCTION_AS_ADOPT, &arg);

  ksyscall(__NR_kill, static_cast<int>(pid), SIGKILL);
  long wret;
  do {
    wret = ksyscall(__NR_wait4, static_cast<int>(pid),
                    static_cast<int *>(nullptr), __WCLONE,
                    static_cast<void *>(nullptr));
  } while (wret == -EINTR);

  if (ret < 0) return MakeError(-ret);
  multiple_as.store(true, std::memory_order_relaxed);
  RegisterAddressSpace(arg.handle);
  (void)ArenaGcSlot(arg.handle, inherited_gc_cursor);
  return static_cast<uint64_t>(arg.handle);
}

size_t AuditLibOSMemory(bool log_details) {
  if (!maps_ref) return 0;

  Status<size_t> n = SnapshotMaps(maps_ref);
  if (!n) return 0;

  size_t bad = 0;
  for (size_t i = 0; i < *n; i++) {
    const Mapping &m = maps_ref[i];
    // The scratch stack for parked clones is private on purpose.
    if (clone_stack_top &&
        m.start ==
            reinterpret_cast<uintptr_t>(clone_stack_top) - kCloneStackSize)
      continue;
    if (IsExemptFromSharing(m)) continue;
    if (!IsLibOSMapping(m)) continue;
    bad++;
    if (log_details)
      LOG(WARN) << "as: LibOS mapping is private and writable: " << std::hex
                << m.start << "-" << m.end << std::dec << " " << m.tag;
  }
  return bad;
}

namespace {

// Is [@start, @end) fully covered by @maps, and with what protections?
//
// Coverage rather than exact-range equality, because the kernel splits and
// merges VMAs freely: the same memory can be one line here and three there
// without anything being wrong. A gap is a real difference; a seam is not.
//
// @maps is in address order, which is the order /proc emits.
bool RangeIsCovered(const Mapping *maps, size_t n, uintptr_t start,
                    uintptr_t end, int prot, bool *prot_differs) {
  uintptr_t cur = start;
  for (size_t i = 0; i < n; i++) {
    if (maps[i].end <= cur) continue;
    if (maps[i].start > cur) return false;  // a gap
    if (maps[i].prot != prot) *prot_differs = true;
    cur = maps[i].end;
    if (cur >= end) return true;
  }
  return false;
}

}  // namespace

namespace {

// Every live address space except @home, into @out (sized kMaxTrackedAS).
// The root is never registered -- it is not created by a clone -- but it is
// an address space like any other and diverges like any other.
size_t CollectOtherAddressSpaces(uint64_t *out, uint64_t home) {
  size_t n = 0;
  {
    rt::SpinGuard g(as_registry.lock);
    for (size_t i = 0; i < as_registry.n; i++)
      if (as_registry.handles[i] != home) out[n++] = as_registry.handles[i];
  }
  if (home != kRootAddressSpace && n < kMaxTrackedAS) out[n++] = kRootAddressSpace;
  return n;
}

}  // namespace

size_t ForEachOtherAddressSpace(void (*fn)(uint64_t, void *), void *ctx) {
  if (!MultipleAddressSpacesExist()) return 0;
  const uint64_t home = GetActiveAddressSpace();

  static uint64_t others[kMaxTrackedAS];
  static rt::Spin others_lock;
  rt::SpinGuard visiting(others_lock);
  size_t n_others = CollectOtherAddressSpaces(others, home);

  size_t visited = 0;
  for (size_t i = 0; i < n_others; i++) {
    // Preemption off for the whole visit: on_sched() rebinds the core to the
    // scheduled thread's address space, so being descheduled here would strand
    // the rest of @fn in somebody else's mappings.
    rt::Preempt preempt;
    rt::PreemptGuard nopreempt(preempt);
    if (!TryActivateAddressSpace(others[i])) continue;
    fn(others[i], ctx);
    ActivateAddressSpace(home);
    visited++;
  }
  return visited;
}

void DumpMappingsOverlapping(uintptr_t start, uintptr_t end) {
  if (!maps_ref) return;
  Status<size_t> n = SnapshotMaps(maps_ref);
  if (!n) return;
  for (size_t i = 0; i < *n; i++) {
    const Mapping &m = maps_ref[i];
    if (m.end <= start || m.start >= end) continue;
    log_err("  OVERLAP %lx-%lx prot=%d shared=%d tag='%s'", m.start, m.end,
            m.prot, m.shared, m.tag);
  }
}

int RangeReadableHere(uintptr_t start, uintptr_t end) {
  if (!maps_ref) return -1;
  Status<size_t> n = SnapshotMaps(maps_ref);
  if (!n) return -1;
  // Readable means covered, with PROT_READ, everywhere in the range. The
  // arena's reservation is PROT_NONE and reads as prot 0, so it does not
  // count; a repaired range does.
  uintptr_t cur = start;
  for (size_t i = 0; i < *n; i++) {
    const Mapping &m = maps_ref[i];
    if (m.end <= cur) continue;
    if (m.start > cur) return 0;
    if (!(m.prot & PROT_READ)) return 0;
    cur = m.end;
    if (cur >= end) return 1;
  }
  return 0;
}

size_t AuditAddressSpaceCoherence(bool log_details) {
  if (!MultipleAddressSpacesExist() || !maps_other) return 0;

  // Take the reference snapshot in whichever address space we are already in.
  Status<size_t> ref_n = SnapshotMaps(maps_ref);
  if (!ref_n) return 0;
  const uint64_t home = GetActiveAddressSpace();

  static uint64_t others[kMaxTrackedAS];  // 8 KB; too big for a runtime stack
  static rt::Spin others_lock;
  rt::SpinGuard visiting(others_lock);
  size_t n_others = CollectOtherAddressSpaces(others, home);

  size_t divergences = 0;
  for (size_t i = 0; i < n_others; i++) {
    size_t missing = 0, prot_differs = 0, extra = 0;
    uintptr_t first_missing = 0, first_extra = 0;
    bool snapshot_ok;
    size_t other_n;

    {
      // Preemption off for the whole visit. on_sched() rebinds the core to the
      // scheduled thread's address space, so being descheduled here would leave
      // the rest of the comparison reading somebody else's mappings -- and
      // would strand this thread in an address space that is not its own.
      rt::Preempt preempt;
      rt::PreemptGuard nopreempt(preempt);
      if (!TryActivateAddressSpace(others[i])) continue;

      Status<size_t> n = SnapshotMaps(maps_other);
      snapshot_ok = !!n;
      other_n = snapshot_ok ? *n : 0;
      ActivateAddressSpace(home);
    }

    if (!snapshot_ok) {
      LOG(WARN) << "as: could not read the map of address space " << others[i];
      continue;
    }

    // Absences: LibOS memory we have that they never received. This is the
    // propagation bug, seen from the other end.
    for (size_t j = 0; j < *ref_n; j++) {
      const Mapping &m = maps_ref[j];
      if (!IsLibOSMapping(m)) continue;
      // Managed regions are *meant* to be absent until touched: that is the
      // lazy repair working, not a divergence.
      if (AddressIsManaged(m.start)) continue;
      bool differs = false;
      if (!RangeIsCovered(maps_other, other_n, m.start, m.end, m.prot,
                          &differs)) {
        if (!missing++) first_missing = m.start;
      } else if (differs) {
        prot_differs++;
      }
    }

    // Extras: LibOS memory they have that we do not. Same bug, opposite
    // direction -- and the one that catches a mapping made by a *child* after
    // the fork, which the reference address space will never see.
    for (size_t j = 0; j < other_n; j++) {
      const Mapping &m = maps_other[j];
      if (AddressIsManaged(m.start)) continue;
      if (!IsLibOSMapping(m)) continue;
      bool differs = false;
      if (!RangeIsCovered(maps_ref, *ref_n, m.start, m.end, m.prot, &differs))
        if (!extra++) first_extra = m.start;
    }

    if (!missing && !prot_differs && !extra) continue;
    divergences += missing + prot_differs + extra;
    if (!log_details) continue;
    LOG(WARN) << "as: address space " << others[i] << " diverges from " << home
              << ": " << missing << " LibOS mapping(s) absent there, " << extra
              << " absent here, " << prot_differs
              << " with different protections";
    if (first_missing)
      LOG(WARN) << "as:   first absent from " << others[i] << ": 0x"
                << std::hex << first_missing << std::dec;
    if (first_extra)
      LOG(WARN) << "as:   first absent from " << home << ": 0x" << std::hex
                << first_extra << std::dec;
  }
  return divergences;
}

namespace {

// Converts the LibOS's private writable mappings into shared ones.
Status<void> ShareLibOSMemory() {
  Status<size_t> n = SnapshotMaps(maps_ref);
  if (!n) return MakeError(n);

  // No guest exists yet, so every writable private mapping is the LibOS's.
  size_t converted = 0, failed = 0, bytes = 0;
  for (size_t i = 0; i < *n; i++) {
    const Mapping &m = maps_ref[i];
    if (IsExemptFromSharing(m)) continue;
    if (ShareOneMapping(m)) {
      converted++;
      bytes += m.end - m.start;
    } else {
      failed++;
      LOG(WARN) << "as: could not share mapping " << std::hex << m.start << "-"
                << m.end << std::dec;
    }
  }

  LOG(INFO) << "as: shared " << converted << " LibOS mappings ("
            << bytes / 1024 << " KB), " << failed << " failed";
  if (failed) return MakeError(EIO);
  return {};
}

}  // namespace

Status<void> InitAddressSpaces() {
  if (!GetCfg().mas_enabled()) {
    LOG(INFO) << "as: per-guest address spaces disabled; fork() will report "
                 "ENOSYS";
    return {};
  }

  // Nothing is done to glibc here any more. Its heap growth, per-thread
  // arenas and large allocations all reach the kernel as mmap from Junction's
  // libc, which seccomp traps and the LibOS arena serves (arena.cc); that
  // memory is repaired on demand in every address space, so there is no
  // reserve to pre-grow and no reason to stop glibc from mapping. The old
  // 64 MB pre-grown heap was the last pre-reserved pool.

  // Before the sweep, so the buffers are shared (and therefore skipped by it),
  // and before any clone, so they exist in every address space later.
  if (!InitScratch(GetCfg().debug_as_audit())) {
    LOG(ERR) << "as: could not allocate the maps scratch buffers";
    return MakeError(ENOMEM);
  }

  Status<void> ret = ShareLibOSMemory();
  if (!ret) {
    LOG(ERR) << "as: failed to share LibOS memory: " << ret.error();
    return ret;
  }

  // Confirm the sweep left nothing behind. A private writable LibOS mapping
  // would quietly diverge the moment an address space is cloned, so it is worth
  // saying so loudly here rather than debugging it later.
  size_t bad = AuditLibOSMemory();
  if (bad)
    LOG(WARN) << "as: " << bad
              << " LibOS mapping(s) are still private; forked processes may "
                 "observe stale LibOS state";

  int fd = ksys_open(JUNCTION_AS_DEVICE, O_RDWR | O_CLOEXEC, 0);
  if (fd < 0) {
    LOG(ERR) << "as: " << JUNCTION_AS_DEVICE << " disappeared: " << Error(-fd);
    return {};
  }
  as_dev_fd = fd;

  void *stack = mmap(nullptr, kCloneStackSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    ksys_close(as_dev_fd);
    as_dev_fd = -1;
    return MakeError(ENOMEM);
  }
  std::memset(stack, 0, kCloneStackSize);
  clone_stack_top = reinterpret_cast<char *>(stack) + kCloneStackSize;

  LOG(INFO) << "as: multiple address spaces enabled";
  return {};
}

}  // namespace junction
