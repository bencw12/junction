// mm.h - memory mapping support

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "junction/base/arch.h"
#include "junction/base/error.h"
#include "junction/base/interval_set.h"
#include "junction/kernel/as.h"
#include "junction/bindings/log.h"
#include "junction/bindings/sync.h"
#include "junction/fs/file.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/ksys.h"
#include "junction/snapshot/cereal.h"

namespace junction {

class MemoryMap;
class Process;

inline constexpr uintptr_t kVirtualAreaMax = 0x500000000000;

constexpr bool AddressValid(void *addr, size_t len) {
  // TODO(amb): maybe check if address is not in the Linux Kernel (negative)?
  return len > 0 && IsPageAligned(reinterpret_cast<uintptr_t>(addr));
}

enum class VMType : int {
  kNormal,  // mapping contains regular anonymous memory
  kHeap,    // mapping is part of the heap (allocated with brk())
  kStack,   // mapping is used as a stack
  kFile,    // mapping is backed by a file
};

// VMArea describes one mapping
struct VMArea {
  VMArea() = default;
  VMArea(void *addr, size_t len, int prot, VMType type)
      : start(reinterpret_cast<uintptr_t>(addr)),
        end(start + len),
        prot(prot),
        type(type) {}
  VMArea(void *addr, size_t len, int prot, std::shared_ptr<File> file,
         off_t offset)
      : start(reinterpret_cast<uintptr_t>(addr)),
        end(start + len),
        prot(prot),
        type(VMType::kFile),
        file(std::move(file)),
        offset(offset) {}

  // Addr returns a pointer to the base address of the VMA.
  void *Addr() const { return reinterpret_cast<void *>(start); }
  // Length returns the length of the VMA.
  size_t Length() const { return end - start; }

  size_t DataLength() const {
    if (type != VMType::kFile) return Length();
    size_t sz = file->get_size();
    if (static_cast<size_t>(offset) > sz) return 0;
    return std::min(PageAlign(sz - offset), Length());
  }

  std::string TypeString() const;

  std::string ProtString() const {
    std::string tmp("---p");
    if (prot & PROT_READ) tmp[0] = 'r';
    if (prot & PROT_WRITE) tmp[1] = 'w';
    if (prot & PROT_EXEC) tmp[2] = 'x';
    return tmp;
  }

  [[nodiscard]] uintptr_t get_start() const { return start; }
  [[nodiscard]] uintptr_t get_end() const { return end; }

  void TrimTail(uintptr_t new_end) {
    assert(end > new_end);
    end = new_end;
  }

  void TrimHead(uintptr_t new_start) {
    assert(start < new_start);
    if (type == VMType::kFile) offset += new_start - start;
    start = new_start;
  }

  bool TryMergeRight(const VMArea &lhs);

  uintptr_t start;
  uintptr_t end;
  int prot;
  bool traced : 1 {false};
  VMType type;
  std::shared_ptr<File> file;
  off_t offset;

  template <class Archive>
  void serialize(Archive &ar) {
    ar(start, end, prot, type, file, offset);
  }
};

std::ostream &operator<<(std::ostream &os, const VMArea &vma);

class PageAccessTracer {
 public:
  PageAccessTracer() = default;

  // Record a new page being accessed
  // Updates the hit time to the earliest time if an existing hit exists.
  // Returns true if this was the first access.
  bool RecordHit(uintptr_t page, Time t, int fault_type) {
    assert(IsPageAligned(page));
    auto [it, inserted] = access_at_.try_emplace(page, t);
    it->second = std::min(it->second, t);
    if (fault_type == PROT_WRITE) {
      auto [it, inserted] = page_writes_.insert(page);
      return inserted;
    }
    return inserted;
  }

  const std::unordered_map<uintptr_t, Time> &get_trace() const {
    return access_at_;
  }

  void Dump(std::ostream &os) const {
    for (const auto &[page_addr, time] : access_at_)
      os << std::dec << time.Microseconds() << ": 0x" << std::hex << page_addr
         << "\n";
    size_t total = access_at_.size();
    size_t writes = page_writes_.size();
    os << "# Total accesses: " << std::dec << total << " writes: " << writes
       << " (" << (writes * 100) / total << "%)\n";
  }

 private:
  std::unordered_map<uintptr_t, Time> access_at_;
  std::unordered_set<uintptr_t> page_writes_;
};

inline std::ostream &operator<<(std::ostream &os,
                                const PageAccessTracer &tracer) {
  tracer.Dump(os);
  return os;
}

// MemoryMap manages memory for a process
// One forked address space and how many memory maps currently live in it.
struct AddressSpaceRef {
  uint64_t handle;
  std::atomic<int> holders;
};

class alignas(kCacheLineSize) MemoryMap {
 public:
  MemoryMap(void *base, size_t len)
      : mm_start_(reinterpret_cast<uintptr_t>(base)),
        mm_end_(mm_start_ + len),
        brk_addr_(mm_start_) {}
  ~MemoryMap();

  // Creates the memory map of a forked child: the same mappings at the same
  // addresses, but in an address space of its own. The caller supplies the
  // address space, which this object takes ownership of.
  // The arena's GC cursor for this address space (arena.h). Read on every
  // syscall entry, so it is a cached pointer into the arena's slot table.
  [[nodiscard]] uint64_t *arena_gc_cursor() const { return arena_gc_cursor_; }

  static std::shared_ptr<MemoryMap> Fork(MemoryMap &parent,
                                         uint64_t as_handle);

  // The address space this memory map's mappings live in.
  [[nodiscard]] uint64_t get_as_handle() const { return as_handle_; }

  // Puts this map in @other's address space, as one more holder of it. exec()
  // uses it: the new image was loaded into whichever address space the caller
  // is bound to, which is @other's -- the caller's own if it forked, its
  // parent's if it vforked. Either way both maps now live there, and the space
  // is released only when the last of its holders is destroyed.
  void ShareAddressSpaceOf(const MemoryMap &other) {
    as_handle_ = other.as_handle_;
    arena_gc_cursor_ = other.arena_gc_cursor_;
    as_ref_ = other.as_ref_;
    if (as_ref_) as_ref_->holders.fetch_add(1, std::memory_order_acq_rel);
  }

  // Makes this map the first holder of a newly created address space, as
  // Fork() does for a child's. exec() uses it when it has to move a process
  // out of an address space it shares.
  void OwnAddressSpace(uint64_t handle);

  // Unmaps this map's mappings in the calling kthread's current address space
  // only, without touching the map's bookkeeping. For an address space that
  // inherited a copy of them -- a clone made for an exec -- and keeps none.
  void UnmapInheritedCopyHere();

  // The range reserved for this memory map.
  [[nodiscard]] AddressRange get_reservation() const {
    return {mm_start_, mm_end_};
  }

  // The ranges reserved for every guest memory map in the system.
  static std::vector<AddressRange> GetReservedRegions();

  [[nodiscard]] std::vector<VMArea> get_vmas();

  // Run a function for each VMA. Runs with the memory map lock held (shared).
  template <typename F>
  void ForEachVMA(F func) {
    rt::ScopedSharedLock g(mu_);
    for (auto const &[end, vma] : vmareas_) func(vma);
  }

  void MarkAsFake() { is_fake_map_ = true; }

  // Free all VMAs from this memory map. Must be called by Exec when replacing
  // one non-reloc binary with another.
  void UnmapAll();

  // SetBreak sets the break address (for the heap). It returns the new address
  // on success, the old address on failure, or EINTR if interrupted.
  Status<uintptr_t> SetBreak(uintptr_t brk_addr);

  // MMap inserts a memory mapping.
  Status<void *> MMap(void *addr, size_t len, int prot, int flags,
                      std::shared_ptr<File> f, off_t off);

  // MMapAnonymous inserts an anonymous memory mapping. MAP_PRIVATE is the
  // default, but a caller asking for MAP_SHARED gets shared memory: setting
  // both bits would mean MAP_SHARED_VALIDATE to the kernel, not "either".
  Status<void *> MMapAnonymous(void *addr, size_t len, int prot, int flags) {
    if (!(flags & MAP_SHARED)) flags |= MAP_PRIVATE;
    return MMap(addr, len, prot, flags | MAP_ANONYMOUS, {}, 0);
  }

  // MProtect changes the access protections of a range of mappings.
  Status<void> MProtect(void *addr, size_t len, int prot);

  // MUnmap removes a range of mappings.
  Status<void> MUnmap(void *addr, size_t len);

  // MAdvise gives the kernel a hint about how a range of mappings will be used.
  Status<void> MAdvise(void *addr, size_t len, int hint);

  // VirtualUsage returns the size (in bytes) of allocated virtual memory.
  [[nodiscard]] size_t VirtualUsage();

  // HeapUsage returns the size (in bytes) of the heap.
  [[nodiscard]] size_t HeapUsage() const { return brk_addr_ - mm_start_; }

  // break_addr
  [[nodiscard]] size_t get_brk_addr() const { return brk_addr_; }

  bool ContainedInMapBounds(void *addr, size_t len) const;

  // LogMappings prints all the mappings to the log.
  void LogMappings();
  std::string GetMappingsString();

  // Start a tracer on this memory map. Sets all permissions in the kernel to
  // PROT_NONE and updates permissions when page faults occur. All threads must
  // be stopped.
  void EnableTracing(Process &p);

  // End tracing. All threads must be stopped or the process must be exiting.
  Status<PageAccessTracer> EndTracing();

  [[nodiscard]] Status<void> DumpTracerReport();

  bool RecordHit(void *addr, size_t len, Time t, int fault_type);

  [[nodiscard]] bool TraceEnabled() const { return !!tracer_; }

  [[nodiscard]] PageAccessTracer &get_tracer() {
    assert(TraceEnabled());
    return *tracer_.get();
  }

  // CheckAccess reports whether [addr, addr + len) is mapped with at least
  // @prot. Junction tracks its guests' mappings, so a bad pointer handed to a
  // system call can be rejected with EFAULT the way Linux rejects it, instead
  // of faulting inside the LibOS.
  [[nodiscard]] bool CheckAccess(const void *addr, size_t len, int prot);

  // Returns true if this page fault is handled by the MM.
  bool HandlePageFault(uintptr_t addr, int required_prot, Time time);

  bool HotPatchInstructions(std::span<const std::byte> src,
                            std::span<std::byte> dst);

  [[nodiscard]] std::string get_bin_path() const;
  [[nodiscard]] std::string get_bin_name() const;
  [[nodiscard]] std::string_view get_cmd_line() const { return cmd_line_; }

  [[nodiscard]] bool is_non_reloc() const { return is_non_reloc_; };

  void mark_non_reloc() {
    assert(!is_non_reloc_);
    is_non_reloc_ = true;
    nr_non_reloc_maps_++;
    (void)ClaimFixedImageSlot();  // whoever got here without asking first
  };

  // An address space has room for one non-relocatable image: they all want the
  // same addresses. The map that holds it owns the address space's slot, from
  // before it loads the image until after its destructor has unmapped it --
  // not "while a process with such a map is on the process list", which is how
  // this used to be decided. That missed a map whose process has exited but
  // which has not been destroyed yet: the next image loaded over its mappings,
  // and its destructor then unmapped them from under the new program ("mm:
  // exec fault at 0x43b000 keeps recurring; the text is not mapped", a compiler
  // dying of SIGSEGV in the middle of make). And it was check-then-act: two
  // vfork children exec'ing at once in their parent's address space both saw
  // it free. make -j does both.
  //
  // Returns false if another map holds the slot of this map's address space.
  [[nodiscard]] bool ClaimFixedImageSlot();
  void ReleaseFixedImageSlot();
  // exec in place: the old map's image is unmapped and the new one's loaded at
  // the same addresses, in the same address space.
  static void TransferFixedImageSlot(MemoryMap &from, MemoryMap &to);

  void set_bin_path(std::shared_ptr<DirectoryEntry> binary_path,
                    const std::vector<std::string_view> &argv) {
    binary_path_ = std::move(binary_path);
    size_t len = 0;
    for (auto &arg : argv) len += arg.size() + 1;
    cmd_line_.reserve(len);
    for (auto &arg : argv) {
      auto ptr = arg.data();
      cmd_line_.insert(cmd_line_.end(), ptr, ptr + arg.size() + 1);
    }
  }

  // True if @addr falls inside any process's reserved region, i.e. it is some
  // guest's memory and belongs in one address space. Anything else is the
  // LibOS's and must be visible in all of them. Uses a try-lock so it is safe
  // to call from the mapping path itself; a contended check reports "guest"
  // rather than risking a deadlock, which only ever loses a report.
  static bool IsInSomeReservation(uintptr_t addr) {
    if (!mm_lock_.TryLock()) return true;
    bool found = mem_areas_.has_overlap(addr, addr + 1);
    mm_lock_.Unlock();
    return found;
  }

  static void RegisterMMRegion(uintptr_t base, size_t len) {
    rt::SpinGuard g(mm_lock_);
    assert(!mem_areas_.has_overlap(base, base + len));
    mem_areas_.Insert({base, base + len});
  }

  // The LibOS's own fixed pages inside the guest range -- the syscall table
  // and the vDSO. Registered so no guest is ever placed over them, and marked
  // so that a clone never excludes them: they must be present in every
  // address space.
  static void RegisterLibOSRegion(uintptr_t base, size_t len) {
    rt::SpinGuard g(mm_lock_);
    assert(!mem_areas_.has_overlap(base, base + len));
    mem_areas_.Insert({base, base + len});
    libos_areas_.Insert({base, base + len});
  }

  // Every guest range other than @own: every other process's slice, fixed
  // image or restored map, from the moment it is allocated until its last
  // mapping is gone. This is what a clone must exclude. It comes from the
  // registry rather than the process list on purpose: a process that has
  // exited leaves the list before its memory map's destructor has finished
  // unmapping its image, and a clone taken in that window would inherit the
  // image without excluding it -- then, once the slice is freed and handed to
  // the next exec in that address space, collide with it.
  static std::vector<AddressRange> GetOtherGuestRegions(AddressRange own);

  [[nodiscard]] static size_t get_nr_non_reloc() { return nr_non_reloc_maps_; }

  static rt::Spin &global_lock() { return mm_lock_; };

  static Status<std::shared_ptr<MemoryMap>> Create(size_t len);

  // Returns the top of the stack that rsp is from if the corresponding VMA was
  // created with MAP_STACK.
  std::optional<void *> GetStackTop(uint64_t rsp) {
    rt::SharedMutexGuard g(mu_);
    auto vma_ref = Find(rsp);
    if (!vma_ref) return std::nullopt;
    const VMArea &vma = *vma_ref;
    if (vma.type == VMType::kStack) return vma.Addr();
    return std::nullopt;
  }

 private:
  friend class cereal::access;
  friend class PageAccessTracer;

  void save(cereal::BinaryOutputArchive &ar) const;
  static void load_and_construct(cereal::BinaryInputArchive &ar,
                                 cereal::construct<MemoryMap> &construct);

  static void FreeMMRegion(uintptr_t start, uintptr_t end) {
    rt::SpinGuard g(mm_lock_);
    if (DropRegionRef(start)) mem_areas_.Clear(start, end);
  }

  // Takes another reference to a reserved range (a fork keeps the addresses).
  static void AcquireRegionRef(uintptr_t start);
  // Drops a reference; returns true when the range may be released.
  static bool DropRegionRef(uintptr_t start);

  static Status<uintptr_t> AllocateMMRegion(size_t len) {
    rt::SpinGuard g(mm_lock_);
    Status<uintptr_t> ret =
        mem_areas_.FindFreeRange(0, len, kVirtualAreaMax, 0);
    if (ret) mem_areas_.Insert({*ret, *ret + len});
    return ret;
  }

  // Clear removes existing VMAreas that overlap with the range [start, end)
  // Ex: ClearMappings(2, 6) when vmareas_ = [1, 3), [5, 7) results in vmareas_
  // = [1, 2), [6, 7). Returns an iterator to the first mapping after the
  // region that was cleared.
  std::map<uintptr_t, VMArea>::iterator Clear(uintptr_t start, uintptr_t end);

  // Find a VMA that contains addr.
  Status<std::reference_wrapper<VMArea>> Find(uintptr_t addr);

  // Modify changes the access protections for memory in the range [start,
  // end).
  void Modify(uintptr_t start, uintptr_t end, int prot);

  // Insert inserts a VMA, removing any overlapping mappings.
  void Insert(VMArea &&vma);

  // Must be called any time is region is being unmapped to potentially updated
  // the global mem_areas_ map.
  void MunmapCheck(void *addr, size_t len);

  rt::SharedMutex mu_;
  const uintptr_t mm_start_;
  const size_t mm_end_;
  uintptr_t brk_addr_;
  ExclusiveIntervalSet<VMArea> vmareas_;
  std::unique_ptr<PageAccessTracer> tracer_;
  std::shared_ptr<DirectoryEntry> binary_path_;
  std::string cmd_line_;
  bool is_non_reloc_{false};
  bool holds_fixed_slot_{false};
  uint64_t fixed_slot_handle_{0};
  // Consecutive "spurious" exec faults on one page; see HandlePageFault.
  static constexpr unsigned kMaxExecFaultRepeats = 64;
  uintptr_t last_exec_fault_{0};
  unsigned exec_fault_repeats_{0};
  bool is_fake_map_{false};  // old ELF snapshot code uses this.
  // The address space these mappings live in. Guest processes that have never
  // forked share the address space Junction started in.
  uint64_t as_handle_{kRootAddressSpace};
  uint64_t *arena_gc_cursor_{nullptr};
  // A forked address space is shared by reference among the processes that
  // live in it: the one that forked it and everything it vfork+execs, each
  // holding one reference through its memory map. The map that drops the last
  // reference releases the space; every other map, on destruction, only
  // removes its own image from it. Null for the root address space, which is
  // never released.
  std::shared_ptr<AddressSpaceRef> as_ref_;
  static rt::Spin mm_lock_;
  static std::atomic_size_t nr_non_reloc_maps_;
  // Tracks all areas allocated in the virtual address space.
  static ExclusiveIntervalSet<SimpleInterval> mem_areas_;
  static ExclusiveIntervalSet<SimpleInterval> libos_areas_;
};

}  // namespace junction
