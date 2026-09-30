// mm.cc - memory management support
//
// The Linux Kernel uses a read-writer mutex to protect VMAs. This can limit
// its concurrency, so we do the same here when performing memory system calls
// to avoid blocking in kthreads. This allows Junction to run other uthreads
// while waiting for system calls to return.
//
// Separately, Junction maintains its own VMAs in userspace. These mirror those
// in the Linux Kernel, but allow each process to track its subset of the
// mappings. VMAs in Junction are also useful for snapshotting processes and
// tearing down process memory.

extern "C" {
#include <sys/mman.h>
#include <sys/sysmacros.h>
}

#include <iomanip>

#include "junction/base/finally.h"
#include "junction/bindings/log.h"
#include "junction/fs/file.h"
#include "junction/kernel/as.h"
#include "junction/kernel/memtrace.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/as.h"
#include "junction/kernel/mm.h"

#include <map>
#include "junction/kernel/proc.h"
#include "junction/kernel/usys.h"

namespace junction {

// Lock protecting allocation of memory maps.
rt::Spin MemoryMap::mm_lock_;

// How many memory maps are using each reserved range. A forked child keeps its
// parent's addresses -- that is the point of fork -- so the range stays
// reserved until the last of them is gone, even though they occupy different
// address spaces.
namespace {
std::map<uintptr_t, unsigned int> mm_region_refs;
}  // namespace
// Set of intervals of allocated memory areas across mem maps. Includes a
// reservation for each memory map as well as memory areas allocated outside of
// these regions.
ExclusiveIntervalSet<SimpleInterval> MemoryMap::mem_areas_;
ExclusiveIntervalSet<SimpleInterval> MemoryMap::libos_areas_;

std::atomic_size_t MemoryMap::nr_non_reloc_maps_{0};

namespace {
rt::Spin fixed_slot_lock;
std::map<uint64_t, const MemoryMap *> fixed_slot_owner;
}  // namespace

bool MemoryMap::ClaimFixedImageSlot() {
  rt::SpinGuard g(fixed_slot_lock);
  if (holds_fixed_slot_ && fixed_slot_handle_ == as_handle_) return true;
  auto [it, fresh] = fixed_slot_owner.try_emplace(as_handle_, this);
  if (!fresh && it->second != this) return false;
  holds_fixed_slot_ = true;
  fixed_slot_handle_ = as_handle_;
  return true;
}

void MemoryMap::ReleaseFixedImageSlot() {
  rt::SpinGuard g(fixed_slot_lock);
  if (!holds_fixed_slot_) return;
  auto it = fixed_slot_owner.find(fixed_slot_handle_);
  if (it != fixed_slot_owner.end() && it->second == this)
    fixed_slot_owner.erase(it);
  holds_fixed_slot_ = false;
}

void MemoryMap::TransferFixedImageSlot(MemoryMap &from, MemoryMap &to) {
  rt::SpinGuard g(fixed_slot_lock);
  if (!from.holds_fixed_slot_) return;
  auto it = fixed_slot_owner.find(from.fixed_slot_handle_);
  if (it == fixed_slot_owner.end() || it->second != &from) return;
  it->second = &to;
  to.holds_fixed_slot_ = true;
  to.fixed_slot_handle_ = from.fixed_slot_handle_;
  from.holds_fixed_slot_ = false;
}

namespace {

constexpr std::pair<uintptr_t, uintptr_t> AddressToBounds(void *addr,
                                                          size_t len) {
  assert(AddressValid(addr, len));
  uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  uintptr_t end = PageAlign(start + len);
  return std::make_pair(start, end);
}

constexpr bool MappingsMergeable(const VMArea &lhs, const VMArea &rhs) {
  // check general merge criteria
  if (lhs.end != rhs.start || lhs.type != rhs.type || lhs.prot != rhs.prot)
    return false;
  // Traced bit is not propagated.
  if (lhs.traced != rhs.traced) return false;
  // check file-specific merge criteria
  if (lhs.type == VMType::kFile) {
    assert(rhs.type == VMType::kFile);
    if (lhs.offset + static_cast<off_t>(lhs.Length()) != rhs.offset)
      return false;
    if (lhs.file != rhs.file) return false;
  }

  return true;
}

bool MappingsValid(const ExclusiveIntervalSet<VMArea> &vmareas) {
  uintptr_t last_end = 0;

  auto it = vmareas.begin();
  auto prev = it;
  while (it != vmareas.end()) {
    const VMArea &vma = it->second;
    if (it->first != vma.end) return false;
    if (vma.start >= vma.end) return false;
    if (!IsPageAligned(vma.start) || !IsPageAligned(vma.end)) return false;
    if (vma.type == VMType::kFile && !vma.file) return false;
    if (vma.type != VMType::kFile && vma.file) return false;
    if (last_end > vma.start) return false;
    if (MappingsMergeable(prev->second, vma)) return false;
    last_end = vma.end;
    prev = it++;
  }

  return true;
}

}  // namespace

bool VMArea::TryMergeRight(const VMArea &lhs) {
  if (!MappingsMergeable(lhs, *this)) return false;
  start = lhs.start;
  offset = lhs.offset;
  return true;
}

std::string VMArea::TypeString() const {
  switch (type) {
    case VMType::kNormal:
      return "";
    case VMType::kHeap:
      return "[heap]";
    case VMType::kStack:
      return "[stack]";
    case VMType::kFile:
      return file->get_filename(FSRoot::GetGlobalRoot());
    default:
      return "";
  }
}

bool MemoryMap::CheckAccess(const void *addr, size_t len, int prot) {
  if (!addr || !len) return false;
  uintptr_t pos = reinterpret_cast<uintptr_t>(addr);
  uintptr_t end = pos + len;
  if (end < pos) return false;  // wrapped

  rt::ScopedSharedLock g(mu_);
  while (pos < end) {
    Status<std::reference_wrapper<VMArea>> vma = Find(pos);
    if (!vma) return false;
    const VMArea &v = *vma;
    if ((v.prot & prot) != prot) return false;
    pos = v.end;
  }
  return true;
}

bool MemoryMap::ContainedInMapBounds(void *addr, size_t len) const {
  auto [start, end] = AddressToBounds(addr, len);
  return start >= mm_start_ && end <= mm_end_;
}

void __noinline MMPanic(ExclusiveIntervalSet<VMArea> *vmareas,
                        ExclusiveIntervalSet<SimpleInterval> &regions) {
  LOG(ERR) << "MM Panic: mappings are not in sync with kernel";
  if (vmareas) {
    LOG(ERR) << "==== VMAs ====";
    for (auto const &[end, vma] : *vmareas) LOG(ERR) << vma;
  }
  LOG(ERR) << "==== Regions ====";
  for (auto const &[end, iv] : regions) LOG(ERR) << iv;
  syscall_exit(-1);
}

void MemoryMap::AcquireRegionRef(uintptr_t start) {
  assert(mm_lock_.IsHeld());
  mm_region_refs[start]++;
}

bool MemoryMap::DropRegionRef(uintptr_t start) {
  assert(mm_lock_.IsHeld());
  auto it = mm_region_refs.find(start);
  if (it == mm_region_refs.end()) return true;
  if (--it->second > 0) return false;
  mm_region_refs.erase(it);
  return true;
}

std::vector<AddressRange> MemoryMap::GetReservedRegions() {
  std::vector<AddressRange> out;
  rt::SpinGuard g(mm_lock_);
  for (auto const &[end, iv] : mem_areas_)
    out.emplace_back(AddressRange{iv.get_start(), iv.get_end()});
  return out;
}

std::vector<AddressRange> MemoryMap::GetOtherGuestRegions(AddressRange own) {
  std::vector<AddressRange> out;
  rt::SpinGuard g(mm_lock_);
  for (auto const &[end, iv] : mem_areas_) {
    // Adjacent slices merge into one interval here, so @own and the LibOS
    // pages have to be cut out of it rather than compared against it.
    std::vector<AddressRange> pieces{{iv.get_start(), iv.get_end()}};
    auto cut = [&pieces](uintptr_t cs, uintptr_t ce) {
      std::vector<AddressRange> next;
      for (const AddressRange &r : pieces) {
        if (ce <= r.start || cs >= r.end) {
          next.push_back(r);
          continue;
        }
        if (r.start < cs) next.push_back({r.start, cs});
        if (ce < r.end) next.push_back({ce, r.end});
      }
      pieces = std::move(next);
    };
    cut(own.start, own.end);
    for (auto const &[lend, liv] : libos_areas_)
      cut(liv.get_start(), liv.get_end());
    out.insert(out.end(), pieces.begin(), pieces.end());
  }
  return out;
}

std::shared_ptr<MemoryMap> MemoryMap::Fork(MemoryMap &parent,
                                           uint64_t as_handle) {
  rt::ScopedSharedLock lock(parent.mu_);
  auto mm = std::make_shared<MemoryMap>(
      reinterpret_cast<void *>(parent.mm_start_),
      parent.mm_end_ - parent.mm_start_);
  mm->as_handle_ = as_handle;
  // The slot was created with the address space (CloneCurrentAddressSpace);
  // this is a lookup. Seeding 0 if it were somehow missing is the safe side.
  mm->arena_gc_cursor_ = ArenaGcSlot(as_handle, 0);
  mm->as_ref_ = std::make_shared<AddressSpaceRef>();
  mm->as_ref_->handle = as_handle;
  mm->as_ref_->holders.store(1, std::memory_order_relaxed);
  mm->brk_addr_ = parent.brk_addr_;
  mm->binary_path_ = parent.binary_path_;
  mm->cmd_line_ = parent.cmd_line_;
  mm->is_non_reloc_ = parent.is_non_reloc_;
  if (mm->is_non_reloc_) nr_non_reloc_maps_++;
  // Its copy of the image is the one in the new address space.
  if (mm->is_non_reloc_) (void)mm->ClaimFixedImageSlot();
  // The child's mappings are the parent's, at the same addresses, in an
  // address space Linux populated with copy-on-write copies of them.
  for (auto const &[end, vma] : parent.vmareas_) {
    VMArea copy = vma;
    mm->vmareas_.Insert(std::move(copy));
  }
  {
    rt::SpinGuard g(mm_lock_);
    AcquireRegionRef(parent.mm_start_);
  }
  return mm;
}

void MemoryMap::OwnAddressSpace(uint64_t handle) {
  as_handle_ = handle;
  arena_gc_cursor_ = ArenaGcSlot(handle, 0);
  as_ref_ = std::make_shared<AddressSpaceRef>();
  as_ref_->handle = handle;
  as_ref_->holders.store(1, std::memory_order_relaxed);
}

void MemoryMap::UnmapInheritedCopyHere() {
  rt::ScopedSharedLock g(mu_);
  // Whatever lies outside the reservation: a non-relocatable image.
  for (auto const &[end, vma] : vmareas_) {
    if (vma.start >= mm_start_ && vma.end <= mm_end_) continue;
    Status<void> ret = KernelMUnmap(vma.Addr(), vma.Length());
    if (!ret) LOG(WARN) << "mm: could not drop inherited copy " << ret.error();
  }
  // And the reservation whole, not just the mappings in it. The PROT_NONE
  // that fills the gaps was cloned too, and left behind it is inherited by
  // every fork from here on -- until the region is handed to someone else and
  // their MAP_FIXED_NOREPLACE finds it occupied ("mm: Create wanted ... got
  // 0", in an address space that never had anything to do with that region).
  Status<void> ret =
      KernelMUnmap(reinterpret_cast<void *>(mm_start_), mm_end_ - mm_start_);
  if (!ret) LOG(WARN) << "mm: could not drop inherited region " << ret.error();
}

Status<std::shared_ptr<MemoryMap>> MemoryMap::Create(size_t len) {
  Status<uintptr_t> base = AllocateMMRegion(len);
  if (!base) return MakeError(base);
  Status<void *> ret = KernelMMap(reinterpret_cast<void *>(*base), len,
                                  PROT_NONE, MAP_FIXED_NOREPLACE);
  if (unlikely(!ret || *ret != reinterpret_cast<void *>(*base))) {
    LOG(ERR) << "mm: Create wanted 0x" << std::hex << *base << "-0x"
             << (*base + len) << " got "
             << (ret ? reinterpret_cast<uintptr_t>(*ret) : 0) << std::dec
             << " in as=" << GetActiveAddressSpace() << " pid="
             << (IsJunctionThread() ? myproc().get_pid() : -1)
             << " err=" << (ret ? 0 : ret.error().code());
    DumpMappingsOverlapping(*base, *base + len);
    if (*ret != reinterpret_cast<void *>(*base)) MMPanic(nullptr, mem_areas_);
    MemoryMap::FreeMMRegion(*base, *base + len);
    return MakeError(ret);
  }
  {
    rt::SpinGuard g(mm_lock_);
    AcquireRegionRef(*base);
  }
  auto mm = std::make_shared<MemoryMap>(*ret, len);
  // The reservation above went into the address space this core is bound to,
  // and that is where this map lives until exec says otherwise. It has to be
  // recorded now, not once the exec has succeeded: a map abandoned because the
  // exec failed -- ENOEXEC is routine, shells try and fall back -- is destroyed
  // with whatever handle it has, and with the default (the root's) its
  // destructor unmapped the reservation from the root address space and left
  // it behind in the caller's, for the next process given this region to
  // collide with ("mm: Create wanted ... got 0", in a forked guest).
  mm->as_handle_ = GetActiveAddressSpace();
  mm->arena_gc_cursor_ = ArenaGcSlot(mm->as_handle_, 0);
  return mm;
}

void MemoryMap::MunmapCheck(void *addr, size_t len) {
  auto [start, end] = AddressToBounds(addr, len);

  if (start >= mm_start_ && end <= mm_end_) return;

  rt::UniqueLock gl(mm_lock_, rt::DeferLock);
  if (!TraceEnabled()) gl.Lock();

  if (start < mm_start_) mem_areas_.Clear(start, std::min(end, mm_start_));

  if (end > mm_end_) mem_areas_.Clear(std::max(mm_end_, start), end);
}

void MemoryMap::UnmapAll() {
  rt::ScopedLock g(mu_);
  for (auto const &[end, vma] : vmareas_) {
    Status<void> ret = KernelMUnmap(vma.Addr(), vma.Length());
    if (!ret) LOG(ERR) << "mm: munmap failed with error " << ret.error();
    MunmapCheck(vma.Addr(), vma.Length());
  }
  vmareas_.clear();
}

MemoryMap::~MemoryMap() {
  if (is_non_reloc_) nr_non_reloc_maps_--;
  if (is_fake_map_) return;

  // The last holder of a forked address space: the whole space is going away,
  // and Linux tears down every mapping with it, so there is nothing to unmap.
  // Do not leave this core bound to it -- whatever runs next here, in
  // particular MemoryMap::Create's MAP_FIXED_NOREPLACE, must be looking at a
  // live address space, not a dead one's still-loaded mappings.
  if (as_ref_ &&
      as_ref_->holders.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    // Binding and the per-core record of it change together, or not at all: a
    // migration between the ioctl and the store leaves some other core
    // recording the root space while bound elsewhere, and the next thread
    // there that wants the root skips the switch and runs where its memory
    // does not exist ("fault ... as=0 ... kernel_as=<other>").
    rt::Preempt::Lock();
    if (GetActiveAddressSpace() == as_handle_)
      ActivateAddressSpace(kRootAddressSpace);
    rt::Preempt::Unlock();
    ReleaseAddressSpace(as_handle_);
    ReleaseFixedImageSlot();
    MemoryMap::FreeMMRegion(mm_start_, mm_end_);
    return;
  }

  // The address space lives on: it is the root's, or other maps still hold it
  // -- the successor after our own exec, the parent after a vfork child's
  // exec, the vfork children after the parent's exit. Our image has to be
  // removed from it, or it outlives this slice's registration and collides
  // with the next process handed these addresses ("mm: Create wanted ... got
  // 0"). Seen under make -j6: a forked shell's pipeline children, vfork+exec'd
  // into the shell's address space, used to inherit ownership of it, so the
  // first to exit released the space under the shell and left its image
  // behind.
  //
  // Unmapping acts on whichever address space this core is bound to, so make
  // sure that is the one these mappings actually live in.
  //
  // After exec() hands its address space to a successor, this map can outlive
  // that successor: it is destroyed whenever its last reference drops, which
  // may be long after the new image has exited and released the space. If it
  // is already gone then so are these mappings -- Linux tore them down with
  // it -- and only the reservation is left to release.
  //
  // This is a visit, and has to end as one. The destructor runs on whichever
  // thread drops the last reference -- a parent reaping a child, say, living
  // in a different address space from these mappings. Left bound to ours, it
  // goes on executing where its own text and data do not exist: an exec fault
  // that never resolves, then a segfault in its next system call ("as=0
  // want_as=N"). And preemption stays off for the duration: a reschedule in
  // the middle has on_sched() rebind the core to the running thread's space,
  // and the rest of these munmaps would land on live memory there.
  rt::Preempt::Lock();
  const uint64_t resume = GetActiveAddressSpace();
  if (!TryActivateAddressSpace(as_handle_)) {
    rt::Preempt::Unlock();
    ReleaseFixedImageSlot();
    MemoryMap::FreeMMRegion(mm_start_, mm_end_);
    return;
  }

  for (auto const &[end, vma] : vmareas_) {
    if (ContainedInMapBounds(vma.Addr(), vma.Length())) continue;
    Status<void> ret = KernelMUnmap(vma.Addr(), vma.Length());
    if (!ret) LOG(ERR) << "mm: munmap failed with error " << ret.error();
    MunmapCheck(vma.Addr(), vma.Length());
  }
  Status<void> ret =
      KernelMUnmap(reinterpret_cast<void *>(mm_start_), mm_end_ - mm_start_);
  if (!ret) LOG(ERR) << "mm: munmap failed with error " << ret.error();

  if (resume != as_handle_ && !TryActivateAddressSpace(resume))
    ActivateAddressSpace(kRootAddressSpace);
  rt::Preempt::Unlock();
  // Only now, with the image unmapped, may another one be loaded here.
  ReleaseFixedImageSlot();
  MemoryMap::FreeMMRegion(mm_start_, mm_end_);
}

std::map<uintptr_t, VMArea>::iterator MemoryMap::Clear(uintptr_t start,
                                                       uintptr_t end) {
  assert(mu_.IsHeld());
  auto it = vmareas_.Clear(start, end);
  assert(MappingsValid(vmareas_));
  return it;
}

Status<std::reference_wrapper<VMArea>> MemoryMap::Find(uintptr_t addr) {
  assert(mu_.IsHeld());
  assert(MappingsValid(vmareas_));
  return vmareas_.Find(addr);
}

void MemoryMap::EnableTracing(Process &p) {
  assert(&p.get_mem_map() == this);
  {
    rt::ScopedLock g(mu_);

    tracer_.reset(new PageAccessTracer());
    for (auto &[end, vma] : vmareas_) {
      vma.traced = true;
      if (vma.prot == PROT_NONE) continue;
      Status<void> ret = KernelMProtect(vma.Addr(), vma.Length(), PROT_NONE);
      if (unlikely(!ret))
        LOG(WARN) << "tracer could not mprotect " << ret.error() << " " << vma;
    }

    memfs::MemFSStartTracer(*FSRoot::GetGlobalRoot().get_root().get());
  }

  std::vector<struct rseq *> rseq_ptrs;
  rseq_ptrs.reserve(p.thread_count());
  p.ForEachThread([&](Thread &th) {
    RseqState &rsstate = th.get_rseq();
    struct rseq *rs = rsstate.get_rseq();
    if (rs) rseq_ptrs.push_back(rs);
  });

  for (auto &p : rseq_ptrs) {
    RecordHit(p, sizeof(*p), Time::Now(), PROT_WRITE);
    std::byte *start = PageAlignDown(p);
    std::byte *end = PageAlign(p + 1);
    Status<void> ret =
        KernelMProtect(start, end - start, PROT_READ | PROT_WRITE);
    if (unlikely(!ret))
      LOG(WARN) << "tracer could not mprotect rseq" << ret.error();
  }
}

Status<PageAccessTracer> MemoryMap::EndTracing() {
  rt::ScopedLock g(mu_);
  if (!tracer_) return MakeError(ENODATA);

  memfs::MemFSEndTracer();

  vmareas_.Modify(
      0, UINT64_MAX, [](const VMArea &) { return true; },
      [](VMArea &vma) {
        if (!vma.traced) return;
        vma.traced = false;
        if (vma.prot != PROT_NONE) {
          Status<void> ret = KernelMProtect(vma.Addr(), vma.Length(), vma.prot);
          if (unlikely(!ret))
            LOG(WARN) << "tracer could not mprotect " << ret.error() << " "
                      << vma;
        }
      });

  PageAccessTracer pt = std::move(*tracer_);
  tracer_.reset();
  assert(MappingsValid(vmareas_));
  return std::move(pt);
}

Status<void> MemoryMap::DumpTracerReport() {
  Status<PageAccessTracer> report = EndTracing();
  if (likely(!report)) return {};

  std::string pth = GetCfg().GetArg("mem-trace-out");
  if (pth.empty()) {
    LOG(INFO) << "memory trace:\n" << *report;
  } else {
    LOG(INFO) << "dumping memory trace to " << pth;
    Status<KernelFile> ord_file =
        KernelFile::Open(pth, O_CREAT | O_TRUNC, FileMode::kWrite, 0644);
    if (unlikely(!ord_file)) {
      LOG(WARN) << "failed to open ord file `" << pth
                << "`: " << ord_file.error();
      return MakeError(ord_file);
    }

    rt::RuntimeLibcGuard g;
    std::stringstream ord;
    ord << *report;
    std::string ordstr = ord.str();
    Status<void> ret = WriteFull(*ord_file, std::as_bytes(std::span{ordstr}));
    if (unlikely(!ret)) {
      LOG(WARN) << "failed to write memory trace: " << ret.error();
      return MakeError(ret);
    }
  }
  return {};
}

bool MemoryMap::RecordHit(void *addr, size_t len, Time time,
                          int required_prot) {
  assert(tracer_);
  uintptr_t page = PageAlignDown(reinterpret_cast<uintptr_t>(addr));
  uintptr_t end = PageAlign(reinterpret_cast<uintptr_t>(addr) + len);
  bool first_hit = false;
  rt::ScopedLock ul(mu_);
  for (; page < end; page += kPageSize)
    first_hit |= tracer_->RecordHit(page, time, required_prot);
  return first_hit;
}

bool MemoryMap::HotPatchInstructions(std::span<const std::byte> src,
                                     std::span<std::byte> dst) {
  if (TraceEnabled()) return false;

  assert(src.size_bytes() == dst.size_bytes());

  std::byte *page_start = PageAlignDown(dst.data());
  std::byte *page_end = PageAlign(dst.data() + dst.size());

  rt::ScopedLock l(mu_);
  auto vma_ref = Find(reinterpret_cast<uintptr_t>(page_start));
  if (unlikely(!vma_ref)) {
    LOG(WARN) << "couldn't find VMA for page " << page_start;
    return false;
  }

  VMArea &vma = *vma_ref;

  if (unlikely(vma.get_end() < reinterpret_cast<uintptr_t>(page_end))) {
    LOG_ONCE(INFO) << "Hotpatch: instructions spanning VMAs";
    return false;
  }

  Status<void> ret =
      KernelMProtect(page_start, page_end - page_start, PROT_READ | PROT_WRITE);
  BUG_ON(!ret);

  std::memcpy(dst.data(), src.data(), src.size_bytes());

  ret = KernelMProtect(page_start, page_end - page_start, vma.prot);
  BUG_ON(!ret);
  return true;
}

// This function is called when there is a SIGSEGV and
// the tracer is detected to be running
//
// Note: the tracer is implemented by mapping all regions as PROT_NONE
// and subsequently intercepting the SIGSEGVs
bool MemoryMap::HandlePageFault(uintptr_t addr, int required_prot, Time time) {
  addr = PageAlignDown(addr);

  rt::UniqueLock g(mm_lock_, rt::DeferLock);
  if (TraceEnabled()) {
    g.Lock();
    // In tracer mode, holding the global MM spin lock implies ownership of the
    // shared mutex (acquire the shared mutex anyways, so that we don't fail
    // debug asserts).
    assert(!mu_.IsHeld());
  }

  rt::ScopedLock l(mu_);

  rt::RuntimeLibcGuard guard;

  auto vma_ref = Find(addr);
  if (unlikely(!vma_ref)) {
    DLOG(WARN) << "couldn't find VMA for page " << (void *)addr;
    return false;
  }

  VMArea &vma = *vma_ref;

  // Checks if the type of access (@required_prot) is legal for this VMA. If the
  // access is invalid, don't record it since no data is accessed.
  if ((vma.prot & required_prot) == 0) return false;

  // Exec fault
  if (!tracer_ && vma.prot == (PROT_READ | PROT_EXEC) &&
      required_prot == PROT_EXEC) {
    // Nothing is repaired here: the fault is declared spurious and the
    // instruction retried. If the text really is missing that never ends --
    // one guest instruction refaulting forever on a core. Let a page be
    // retried a few times, then let the process take its SIGSEGV.
    if (addr == last_exec_fault_) {
      if (++exec_fault_repeats_ > kMaxExecFaultRepeats) {
        LOG(ERR) << "mm: exec fault at " << (void *)addr
                 << " keeps recurring; the text is not mapped in as="
                 << GetActiveAddressSpace() << " (want " << as_handle_ << ")";
        return false;
      }
    } else {
      last_exec_fault_ = addr;
      exec_fault_repeats_ = 0;
    }
    return true;
  }

  if (!tracer_) return false;

  // An mprotect may have forced us to remap an already touched page as
  // PROT_NONE. Record the hit regardless and restore permissions.
  tracer_->RecordHit(addr, time, required_prot);

  int reprot = vma.prot;
  // Don't restore write permissions unless this fault was a write.
  if (required_prot != PROT_WRITE) reprot &= ~PROT_WRITE;

  Status<void> ret =
      KernelMProtect(reinterpret_cast<void *>(addr), kPageSize, reprot);
  if (unlikely(!ret)) {
    LOG(ERR) << " failed to restore permission to page" << ret.error();
    return false;
  }

  return true;
}

// An mprotect during tracing may downgrade the permissions of a region. We need
// to immediately remove those permissions from all pages in the region,
// however, we don't want to effectively add back any permissions to pages that
// are marked as PROT_NONE because they haven't been accessed yet. Instead we
// just mark the entire region PROT_NONE, potentially allowing pages that have
// already been faulted in to fault again.
void __attribute__((cold)) TracerModifyProt(VMArea &vma, int new_prot) {
  int old_prot = std::exchange(vma.prot, new_prot);

  // Non traced VMAs should be updated immediately.
  if (!vma.traced) {
    Status<void> ret = KernelMProtect(vma.Addr(), vma.Length(), new_prot);
    if (unlikely(!ret))
      LOG(WARN) << "tracer could not mprotect " << ret.error() << " " << vma;
    return;
  }

  // check if old protections have bits that are not present in the new
  // protections. If we are not modifying the protection or only adding
  // protection bits, then we will rely on the fault handler to apply those
  // permissions page-wise.
  if ((old_prot & ~new_prot) == 0) return;

  Status<void> ret = KernelMProtect(vma.Addr(), vma.Length(), PROT_NONE);
  if (unlikely(!ret))
    LOG(WARN) << "tracer could not mprotect " << ret.error() << " " << vma;
}

void MemoryMap::Modify(uintptr_t start, uintptr_t end, int prot) {
  assert(mu_.IsHeld());
  // TODO(amb): Should this function fail if there are unmapped gaps?

  vmareas_.Modify(
      start, end, [&](const VMArea &vma) { return vma.prot != prot; },
      [&](VMArea &vma) {
        if (unlikely(TraceEnabled()))
          TracerModifyProt(vma, prot);
        else
          vma.prot = prot;
      });

  assert(MappingsValid(vmareas_));
}

void MemoryMap::Insert(VMArea &&vma) {
  assert(mu_.IsHeld());
  vmareas_.Insert(std::move(vma));
  assert(MappingsValid(vmareas_));
}

std::vector<VMArea> MemoryMap::get_vmas() {
  std::vector<VMArea> tmp;
  rt::ScopedSharedLock g(mu_);
  tmp.reserve(vmareas_.size());
  for (auto const &[end, vma] : vmareas_) tmp.push_back(vma);
  return tmp;
}

std::string MemoryMap::get_bin_path() const {
  if (!binary_path_) return "[STALE]";
  std::string out;
  out.reserve(PATH_MAX);
  rt::RuntimeLibcGuard g;
  std::ostringstream ss(std::move(out));
  Status<void> ret = binary_path_->GetFullPath(myproc_or_global_fs(), ss);
  if (unlikely(!ret)) return "[STALE]";
  return ss.str();
}

std::string MemoryMap::get_bin_name() const {
  if (!binary_path_) return "[STALE]";
  return binary_path_->get_name();
}

Status<uintptr_t> MemoryMap::SetBreak(uintptr_t brk_addr) {
  // NOTE: Must save the unaligned address, but the mapping will still be
  // aligned to a page boundary.
  uintptr_t newbrk = brk_addr;

  // Return the current brk address if out of range.
  if (newbrk < mm_start_ || newbrk >= mm_end_) return brk_addr_;

  // Otherwise, try to adjust the brk address.
  rt::UniqueLock ul(mu_);

  uintptr_t oldbrk = brk_addr_;

  // Stop here if the mapping has not changed after alignment.
  if (PageAlign(oldbrk) == PageAlign(newbrk)) {
    brk_addr_ = newbrk;
    return brk_addr_;
  }

  // Make sure we don't overlap with an mmapped region.
  auto vma_ref = vmareas_.UpperBound(brk_addr_);
  if (vma_ref && vma_ref.value().get().start < PageAlign(newbrk))
    return brk_addr_;

  if (newbrk > oldbrk) {
    // Grow the heap mapping.
    void *addr = reinterpret_cast<void *>(PageAlign(oldbrk));
    size_t len = PageAlign(newbrk) - PageAlign(oldbrk);
    Status<void> ret = KernelMMapFixed(addr, len, PROT_READ | PROT_WRITE, 0);
    if (!ret) {
      LOG(ERR) << "mm: growing brk address failed. " << ret.error();
      return brk_addr_;
    }
    Insert(VMArea(addr, len, PROT_READ | PROT_WRITE, VMType::kHeap));
  } else {
    void *addr = reinterpret_cast<void *>(PageAlign(newbrk));
    size_t len = PageAlign(oldbrk) - PageAlign(newbrk);
    // Shrink the heap mapping.
    Status<void> ret = KernelMMapFixed(addr, len, PROT_NONE, 0);
    if (!ret) {
      LOG(ERR) << "mm: shrinking brk address failed. " << ret.error();
      return brk_addr_;
    }
    auto [start, end] = AddressToBounds(addr, len);
    Clear(start, end);
  }

  // Success; update brk address and return.
  brk_addr_ = newbrk;
  return brk_addr_;
}

Status<void *> MemoryMap::MMap(void *addr, size_t len, int prot, int flags,
                               std::shared_ptr<File> f, off_t off) {
  // Shared anonymous mappings are real shared memory now that guests have
  // separate address spaces. Shared file mappings still are not.
  if ((flags & MAP_SHARED) != 0 && (flags & MAP_ANONYMOUS) == 0) {
    LOG_ONCE(ERR) << "mm: shared file mmap() mappings are unsupported";
    // FIXME(amb): Java requires us to continue here to run
    // return MakeError(EINVAL);
  }

  // check length and alignment
  if (!AddressValid(addr, len)) return MakeError(EINVAL);

  // check if pages should be populated (i.e., read-ahead faulted)
  bool do_populate = (flags & MAP_POPULATE);
  flags &= ~MAP_POPULATE;

  // length must be aligned up to the nearest page
  len = PageAlign(len);

  // do the mapping
  Status<void *> raddr;
  bool reserved_from_global_pool = false;
  {
    rt::UniqueLock ul(mu_);

    if (!(flags & MAP_FIXED)) {
      Status<uintptr_t> ret{MakeError(1)};

      auto [hint_start, hint_end] = AddressToBounds(addr, len);
      if (addr && (hint_end > mm_end_ || hint_start < mm_start_)) {
        // User wants an address range outside of this MM's address range. Try
        // to accomodate it by allocating from the global address pool.
        rt::UniqueLock gl(mm_lock_, rt::DeferLock);
        // If trace is running we already acquired the lock.
        if (!TraceEnabled()) gl.Lock();
        assert(mm_lock_.IsHeld());

        // TODO: what is the right upper bound here?
        ret = mem_areas_.FindFreeRange(hint_start, len, kVirtualAreaMax, 0);
        if (ret) {
          mem_areas_.Insert({*ret, *ret + len});
          reserved_from_global_pool = true;
          // There should be NO existing mapping.
          flags |= MAP_FIXED_NOREPLACE;
        }
      }

      if (!ret) {
        ret = vmareas_.FindFreeRange(hint_start, len, mm_end_, brk_addr_);
        if (!ret) return MakeError(ret);
      }

      addr = reinterpret_cast<void *>(*ret);
      flags |= MAP_FIXED;
    }

    // Free an allocation from the global pool on error.
    auto clean_on_err = finally([&] {
      if (!reserved_from_global_pool) return;
      rt::UniqueLock gl(mm_lock_, rt::DeferLock);
      if (!TraceEnabled()) gl.Lock();
      mem_areas_.Clear(reinterpret_cast<uintptr_t>(addr),
                       reinterpret_cast<uintptr_t>(addr) + len);
    });

    VMArea vma;
    if (flags & MAP_ANONYMOUS) {
      // anonymous memory
      raddr = KernelMMap(addr, len, prot, flags);
      if (!raddr) {
        if (raddr.error() == EEXIST) MMPanic(&vmareas_, mem_areas_);
        return MakeError(raddr);
      }
      VMType type = (flags & MAP_STACK) != 0 ? VMType::kStack : VMType::kNormal;
      vma = VMArea(*raddr, len, prot, type);
    } else {
      // file-backed memory (Linux ignores MAP_FILE)
      if (!f) return MakeError(EBADF);
      raddr = f->MMap(addr, len, prot, flags, off);
      if (!raddr) {
        if (raddr.error() == EEXIST) MMPanic(&vmareas_, mem_areas_);
        return MakeError(raddr);
      }
      vma = VMArea(*raddr, len, prot, std::move(f), off);
    }

    clean_on_err.Dismiss();
    Insert(std::move(vma));
  }

  // populate the mapping (if requested)
  if (do_populate) {
    Status<void> ret = MAdvise(*raddr, len, MADV_POPULATE_READ);
    if (!ret && ret.error() == EINTR) return MakeError(EINTR);
  }

  return *raddr;
}

Status<void> MemoryMap::MProtect(void *addr, size_t len, int prot) {
  // check length and alignment
  if (!AddressValid(addr, len)) return MakeError(EINVAL);

  // change protections
  rt::UniqueLock ul(mu_);

  // Modify() will make KernelMProtect calls if the tracer is on.
  if (likely(!TraceEnabled())) {
    Status<void> ret = KernelMProtect(addr, len, prot);
    if (!ret) return MakeError(ret);
  }

  auto [start, end] = AddressToBounds(addr, len);
  Modify(start, end, prot);
  return {};
}

Status<void> MemoryMap::MUnmap(void *addr, size_t len) {
  // check length and alignment
  if (!AddressValid(addr, len)) return MakeError(EINVAL);

  // clear mappings
  {
    // A plain lock, not InterruptOrLock: Linux never fails mmap, munmap,
    // mprotect, madvise or brk with EINTR, and programs are written to that.
    // glibc frees a joined thread's stack with munmap() and aborts on *any*
    // failure (nptl/nptl-stack.c), so an interruptible lock here turned a
    // signal arriving during pthread_join() into SIGABRT. Same change in
    // SetBreak, MMap, MProtect and MAdvise.
    rt::UniqueLock ul(mu_);
    // Note: we may need to map a PROT_NONE region to prevent Linux from placing
    // other VMAs here.
    Status<void> ret = KernelMUnmap(addr, len);
    if (!ret) return MakeError(ret);
    auto [start, end] = AddressToBounds(addr, len);
    Clear(start, end);
  }

  MunmapCheck(addr, len);

  return {};
}

Status<void> MemoryMap::MAdvise(void *addr, size_t len, int hint) {
  // check length and alignment
  if (!AddressValid(addr, len)) return MakeError(EINVAL);

  // Translate MADV_FREEs to MADV_DONTNEED to immediately zero a page.
  if (hint == MADV_FREE && GetCfg().madv_dontneed_remap()) hint = MADV_DONTNEED;

  // provide mapping hints
  rt::SharedLock ul(mu_);
  return KernelMAdvise(addr, len, hint);
}

size_t MemoryMap::VirtualUsage() {
  size_t usage = 0;
  rt::ScopedSharedLock g(mu_);
  for (auto const &[end, vma] : vmareas_) usage += vma.Length();
  return usage;
}

void MemoryMap::save(cereal::BinaryOutputArchive &ar) const {
  ar(mm_start_, mm_end_ - mm_start_, brk_addr_, cmd_line_);
  ar(vmareas_);
  if (!binary_path_ || binary_path_->WillBeSerialized())
    ar(true, binary_path_);
  else
    ar(false, get_bin_path());
}

void MemoryMap::load_and_construct(cereal::BinaryInputArchive &ar,
                                   cereal::construct<MemoryMap> &construct) {
  uintptr_t mm_start, len;
  ar(mm_start, len);

  RegisterMMRegion(mm_start, len);

  Status<void *> ret =
      KernelMMap(reinterpret_cast<void *>(mm_start), len, PROT_NONE, 0);
  if (!ret) throw std::bad_alloc();

  construct(*ret, len);
  ar(construct->brk_addr_, construct->cmd_line_);
  ar(construct->vmareas_);

  bool hasdent;
  ar(hasdent);
  if (hasdent) {
    ar(construct->binary_path_);
  } else {
    std::string path;
    ar(path);
    Status<std::shared_ptr<DirectoryEntry>> dent =
        LookupDirEntry(FSRoot::GetGlobalRoot(), path);
    if (dent) construct->binary_path_ = std::move(*dent);
  }
}

std::ostream &operator<<(std::ostream &os, const VMArea &vma) {
  uintptr_t offset = 0;
  ino_t inum = 0;
  int dev_major = 0;
  int dev_minor = 0;
  if (vma.type == VMType::kFile) {
    offset = vma.offset;
    struct stat buf;
    if (vma.file->Stat(&buf)) {
      inum = buf.st_ino;
      dev_major = major(buf.st_dev);
      dev_minor = minor(buf.st_dev);
    }
  }

  return os << std::hex << vma.start << "-" << vma.end << " "
            << vma.ProtString() << " " << std::setfill('0') << std::setw(8)
            << std::right << offset << " " << std::setw(2) << dev_major << ":"
            << std::setw(2) << dev_minor << " " << std::dec << std::setfill(' ')
            << std::setw(26) << std::left << inum << " " << vma.TypeString();
}

std::string MemoryMap::GetMappingsString() {
  rt::ScopedSharedLock g(mu_);
  rt::RuntimeLibcGuard r;
  std::ostringstream ss;
  for (auto const &[end, vma] : vmareas_) ss << vma << "\n";
  return ss.str();
}

void MemoryMap::LogMappings() {
  rt::ScopedSharedLock g(mu_);
  for (auto const &[end, vma] : vmareas_) LOG(INFO) << vma;
}

template <typename Callable, typename... Args>
auto __attribute__((cold)) TracerGuardMMDoCall(Callable &&func, Args &&...args)
  requires std::invocable<Callable, Args...>
{
  return CallOnSyscallStack([&] {
    rt::SpinGuard g(MemoryMap::global_lock());
    return std::forward<Callable>(func)(std::forward<Args>(args)...);
  });
}

template <typename Callable, typename... Args>
inline auto TracerGuardCheck(MemoryMap &mm, Callable &&func, Args &&...args)
  requires std::invocable<Callable, Args...>
{
  if (likely(!mm.TraceEnabled()))
    return std::forward<Callable>(func)(std::forward<Args>(args)...);

  return TracerGuardMMDoCall(std::forward<Callable>(func),
                             std::forward<Args>(args)...);
}

intptr_t usys_brk(void *addr) {
  MemoryMap &mm = myproc().get_mem_map();
  Status<uintptr_t> ret = TracerGuardCheck(
      mm, [&] { return mm.SetBreak(reinterpret_cast<uintptr_t>(addr)); });
  if (!ret) return MakeCErrorRestartSys(ret);
  return *ret;
}

intptr_t usys_mmap(void *addr, size_t len, int prot, int flags, int fd,
                   off_t offset) {
  MemoryMap &mm = myproc().get_mem_map();

  // Shared anonymous memory used to be quietly downgraded to private: with
  // every guest in one address space there was only ever one copy, so nothing
  // could tell the difference. Once a process can fork, the difference is
  // exactly what MAP_SHARED is for, so honour it.
  if ((flags & MAP_SHARED) && fd == -1) {
    flags |= MAP_ANONYMOUS;
    if (!MultiAddressSpaceEnabled()) flags &= ~MAP_SHARED;
  } else if ((flags & MAP_SHARED_VALIDATE) == MAP_SHARED_VALIDATE) {
    // MAP_SHARED_VALIDATE happens to be (MAP_ANONYMOUS | MAP_SHARED). Remove
    // the anonymous flag so we hit the correct case below. Note that if fd is
    // -1 then we hit the case above instead of here.
    flags &= ~MAP_ANONYMOUS;
  }

  // Map anonymous memory.
  if ((flags & MAP_ANONYMOUS) != 0) {
    Status<void *> ret = TracerGuardCheck(
        mm, [&] { return mm.MMapAnonymous(addr, len, prot, flags); });
    if (unlikely(MemTraceEnabled()))
      MemTraceMap(MemTraceSource::kGuestSyscall, "mmap",
                  reinterpret_cast<uintptr_t>(addr), len, prot, flags, fd,
                  offset, 0,
                  ret ? reinterpret_cast<int64_t>(*ret) : -1);
    if (!ret) return MakeCErrorRestartSys(ret);
    return reinterpret_cast<intptr_t>(*ret);
  }

  // Map a file.
  FileTable &ftbl = myproc().get_file_table();
  std::shared_ptr<File> f = ftbl.Dup(fd);
  if (!f) return -EBADF;
  Status<void *> ret = TracerGuardCheck(mm, [&] {
    return mm.MMap(addr, len, prot, flags, std::move(f), offset);
  });
  if (!ret) return MakeCErrorRestartSys(ret);
  return reinterpret_cast<intptr_t>(*ret);
}

// mremap for guest memory. Shrinking unmaps the tail in place. Growing always
// moves: a new anonymous mapping, a copy, and an unmap of the old range, which
// is what MREMAP_MAYMOVE permits and needs nothing from the address-space
// machinery beyond mmap and munmap. Only anonymous memory can grow -- a file
// mapping cannot be rebuilt by copying, and neither could a MAP_SHARED one,
// which the VMA does not distinguish; glibc's realloc and apt's cache, the
// callers that matter, only ever grow private memory. Everything else gets
// ENOMEM, which callers treat as "allocate and copy yourself" -- unlike the
// ENOSYS this used to return, which apt treats as fatal.
long usys_mremap(void *old_addr, size_t old_len, size_t new_len, int flags,
                 [[maybe_unused]] void *new_addr) {
  if (unlikely(flags & ~MREMAP_MAYMOVE)) return -EINVAL;
  if (unlikely(!IsPageAligned(reinterpret_cast<uintptr_t>(old_addr)) ||
               !old_len || !new_len))
    return -EINVAL;
  old_len = PageAlign(old_len);
  new_len = PageAlign(new_len);
  if (new_len == old_len) return reinterpret_cast<intptr_t>(old_addr);

  MemoryMap &mm = myproc().get_mem_map();
  auto *old_p = static_cast<std::byte *>(old_addr);

  if (new_len < old_len) {
    Status<void> ret = TracerGuardCheck(
        mm, [&] { return mm.MUnmap(old_p + new_len, old_len - new_len); });
    if (!ret) return MakeCErrorRestartSys(ret);
    return reinterpret_cast<intptr_t>(old_addr);
  }

  // The old range must lie within one readable anonymous mapping.
  const uintptr_t start = reinterpret_cast<uintptr_t>(old_addr);
  int prot = -1;
  mm.ForEachVMA([&](const VMArea &vma) {
    if (vma.start <= start && start + old_len <= vma.end &&
        vma.type != VMType::kFile)
      prot = vma.prot;
  });
  if (prot == -1) return -EFAULT;
  if (!(flags & MREMAP_MAYMOVE) || !(prot & PROT_READ)) return -ENOMEM;

  Status<void *> dst = TracerGuardCheck(mm, [&] {
    return mm.MMapAnonymous(nullptr, new_len, PROT_READ | PROT_WRITE, 0);
  });
  if (!dst) return MakeCErrorRestartSys(dst);
  std::memcpy(*dst, old_addr, old_len);
  if (prot != (PROT_READ | PROT_WRITE)) {
    Status<void> ret =
        TracerGuardCheck(mm, [&] { return mm.MProtect(*dst, new_len, prot); });
    if (!ret) return MakeCErrorRestartSys(ret);
  }
  Status<void> ret =
      TracerGuardCheck(mm, [&] { return mm.MUnmap(old_addr, old_len); });
  if (!ret) return MakeCErrorRestartSys(ret);
  return reinterpret_cast<intptr_t>(*dst);
}

long usys_mprotect(void *addr, size_t len, int prot) {
  MemoryMap &mm = myproc().get_mem_map();
  Status<void> ret =
      TracerGuardCheck(mm, [&] { return mm.MProtect(addr, len, prot); });
  if (!ret) return MakeCErrorRestartSys(ret);
  return 0;
}

long usys_munmap(void *addr, size_t len) {
  MemoryMap &mm = myproc().get_mem_map();
  Status<void> ret = TracerGuardCheck(mm, [&] { return mm.MUnmap(addr, len); });
  if (!ret) return MakeCErrorRestartSys(ret);
  return 0;
}

long usys_madvise(void *addr, size_t len, int hint) {
  MemoryMap &mm = myproc().get_mem_map();
  Status<void> ret =
      TracerGuardCheck(mm, [&] { return mm.MAdvise(addr, len, hint); });
  if (!ret) return MakeCErrorRestartSys(ret);
  return 0;
}

}  // namespace junction
