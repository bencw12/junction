// memfs.cc - support for memfs inode types

extern "C" {
#include <sys/mman.h>
}

#include "junction/base/bitmap.h"
#include "junction/bindings/log.h"
#include "junction/fs/memfs/memfs.h"
#include "junction/fs/memfs/memfsfile.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/mm.h"

#ifndef MFD_EXEC
#define MFD_EXEC 0
#define MFD_FLAGS 0
#else
#define MFD_FLAGS MFD_EXEC
#endif

namespace junction::memfs {

namespace {

// File descriptor of open memfd used to back memfs files.
int memfs_extent_fd;
// Lock protecting @allocated_file_slots.
rt::Spin file_alloc_lock;
// Bitmap of allocated slots in the memfd area.
//
// One allocator, not two. There used to be a second -- a monotonic address
// counter, next_memfs_faddr -- so a file's address and its memfd offset were
// handed out independently and had no relationship to each other. Measured
// over 40 create/delete cycles of a single file: 1 offset, 40 addresses.
//
// A fault in an extent could therefore not be repaired, because nothing in the
// faulting context could say which offset the address had been bound to. With
// the address derived from the slot, it can:
//
//     addr = memfs_base + slot * kMaxSizeBytes,  slot = offset / kMaxSizeBytes
//
// The counter also never recycled, so 6144 file creations walked off the end
// of the guest region it started in.
bitmap<kMaxFiles> allocated_file_slots;

// Base of the extent region, fixed once at startup.
//
// Each file's extent sits at memfs_base + (its memfd offset), so the binding is
// 1:1 in both directions and a fault anywhere in the region can be repaired by
// arithmetic alone, with no lookup and no lock. See junction/kernel/arena.h.
//
// Not a compile-time constant, because there is no address range that is
// reliably free: the kernel places the LibOS image and its heap wherever ASLR
// decides, and over several runs junction_run's text appeared around
// 0x575e1a723000 and its heap at 0x58b31b64a000 -- both inside the fixed region
// that had been picked for extents. Asking for the region instead gets a hole
// that is provably free, and holding it as one PROT_NONE reservation keeps it
// that way.
uintptr_t memfs_base;

char *SlotToAddr(size_t slot) {
  return reinterpret_cast<char *>(memfs_base + slot * kMaxSizeBytes);
}

// MemIDevice is an inode type for character and block devices
class MemIDevice : public Inode {
 public:
  MemIDevice(dev_t dev, mode_t mode, ino_t inum = AllocateInodeNumber())
      : Inode(mode, inum), dev_(dev) {}

  Status<std::shared_ptr<File>> Open(
      uint32_t flags, FileMode mode,
      std::shared_ptr<DirectoryEntry> dent) override {
    return DeviceOpen(std::move(dent), dev_, flags, mode);
  }
  Status<void> GetStats(struct stat *buf) const override {
    MemInodeToStats(*this, buf);
    buf->st_rdev = dev_;
    return {};
  }
  Status<void> GetStatFS(struct statfs *buf) const override {
    StatFs(buf);
    return {};
  }

  template <class Archive>
  void save(Archive &ar) const {
    ar(dev_, get_mode(), get_inum());
    ar(cereal::base_class<Inode>(this));
  }

  template <class Archive>
  static void load_and_construct(Archive &ar,
                                 cereal::construct<MemIDevice> &construct) {
    dev_t dev;
    mode_t mode;
    ino_t inum;
    ar(dev, mode, inum);
    construct(dev, mode, inum);
    ar(cereal::base_class<Inode>(construct.ptr()));
  }

 private:
  dev_t dev_;
};

}  // namespace

Status<void> SaveMemFs(cereal::BinaryOutputArchive &ar) {
  // The slot bitmap is now the whole state: addresses are derived from it
  // rather than allocated separately, so there is no second counter to save.
  ar(allocated_file_slots);
  return {};
}

Status<void> RestoreMemFs(cereal::BinaryInputArchive &ar) {
  ar(allocated_file_slots);
  return {};
}

MemInode::~MemInode() {
  // Drop the backing pages, and leave the mapping in place.
  //
  // MADV_REMOVE, not MADV_DONTNEED: this is a shared mapping of a memfd, where
  // DONTNEED drops only this address space's PTEs and leaves the page -- and
  // its old contents -- in the file. REMOVE punches the object itself, which
  // propagates through i_mmap to every address space that maps it, actually
  // returns the memory, and is what makes the next user of this offset read
  // zeros. Measured in docs/traces/memfd_reclaim.c.
  //
  // Not unmapping is deliberate, and is what the 1:1 layout buys. The address
  // of an extent is derived from its slot, so the binding for any address in
  // this region is immutable: it is always (memfs_fd, addr - memfs_base, RW),
  // whatever file happens to own the slot. Reusing the slot therefore cannot
  // reuse the address for a *different* binding, which is the condition the
  // whole lazy-repair design rests on (see junction/kernel/arena.h).
  //
  // Unmapping would in fact be the unsafe option. Other address spaces
  // materialise extents on demand and would keep their mappings regardless, so
  // an unmap here would only desynchronise this one -- and it would punch a
  // hole in the reserved region that the next unrelated mmap could be handed.
  if (extent_offset_ != -1) {
    Status<void> ret = KernelMAdvise(buf_, kMaxSizeBytes, MADV_REMOVE);
    if (unlikely(!ret))
      LOG(WARN) << "meminode: failed to remove pages " << ret.error();

    rt::SpinGuard g(file_alloc_lock);
    allocated_file_slots.clear(extent_offset_);
    return;
  }

  // A restored inode owns a private mapping rather than a slot in the extent
  // region, so it is unmapped the ordinary way.
  Status<void> ret = KernelMUnmap(buf_, kMaxSizeBytes);
  if (unlikely(!ret)) LOG(WARN) << "failed to unmap memfs " << ret.error();
}

Status<std::shared_ptr<MemInode>> MemInode::Create(mode_t mode) {
  size_t off;
  {
    rt::SpinGuard g(file_alloc_lock);
    std::optional<size_t> tmp = allocated_file_slots.find_next_clear(0);
    if (unlikely(!tmp)) return MakeError(ENOSPC);
    off = *tmp;
    allocated_file_slots.set(off);
  }

  // MAP_FIXED is safe here precisely because the slot allocator owns this
  // address: nothing else can be mapped at it, and the previous occupant was
  // unmapped from every address space when it was freed.
  char *addr = SlotToAddr(off);
  intptr_t ret = ksys_mmap(addr, kMaxSizeBytes, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, memfs_extent_fd,
                           off * kMaxSizeBytes);
  // A LibOS mapping, made lazily whenever a memfs file is created, into
  // whichever address space is current. It reaches no other address space --
  // that is the bug this whole region exists to make repairable, and the fault
  // handler is what repairs it. Traced here rather than in KernelMMap because
  // this calls ksys_mmap directly.
  TRACE_MEM(kJunctionInternal, "mmap-memfs-extent", addr, kMaxSizeBytes,
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memfs_extent_fd,
            off * kMaxSizeBytes, ret);
  if (unlikely(ret < 0)) {
    rt::SpinGuard g(file_alloc_lock);
    allocated_file_slots.clear(off);
    return MakeError(-ret);
  }
  assert(reinterpret_cast<char *>(ret) == addr);
  return std::make_shared<MemInode>(Token{}, addr, off, mode);
}

Status<void *> MemInode::MMap(void *addr, size_t length, int prot, int flags,
                              off_t off) {
  // TODO(jf): support mapping restored memfs files.
  if (extent_offset_ == -1) return MakeError(EINVAL);

  assert(!(flags & MAP_ANONYMOUS));
  intptr_t ret = ksys_mmap(addr, length, prot, flags, memfs_extent_fd,
                           extent_offset_ * kMaxSizeBytes + off);
  TRACE_MEM(kGuestSyscall, "mmap-memfs", addr,
            length, prot, flags, memfs_extent_fd,
            extent_offset_ * kMaxSizeBytes + off, ret);
  if (unlikely(ret < 0)) return MakeError(-ret);
  return reinterpret_cast<void *>(ret);
}

Status<void> MemInode::SetSize(size_t newlen) {
  if (unlikely(newlen > kMaxSizeBytes)) return MakeError(EINVAL);
  rt::ScopedLock g_(lock_);

  size_t newlen_p = PageAlign(newlen);
  size_t oldlen_p = PageAlign(size_);
  if (newlen_p < oldlen_p) {
    // Zero dropped blocks.
    int advice = extent_offset_ == -1 ? MADV_DONTNEED : MADV_REMOVE;
    Status<void> ret =
        KernelMAdvise(buf_ + newlen_p, oldlen_p - newlen_p, advice);
    if (unlikely(!ret))
      LOG(WARN) << "meminode: failed to remove pages " << ret.error();
  }
  size_ = newlen;
  return {};
}

Status<void> MemInode::GetStats(struct stat *buf) const {
  MemInodeToStats(*this, buf);
  buf->st_size = size_;
  buf->st_blocks = 0;
  return {};
}

Status<std::shared_ptr<File>> MemInode::Open(
    uint32_t flags, FileMode mode, std::shared_ptr<DirectoryEntry> dent) {
  return std::make_shared<MemFSFile>(flags, mode, std::move(dent));
}

std::shared_ptr<ISoftLink> CreateISoftLink(std::string path) {
  return std::make_shared<MemISoftLink>(std::move(path));
}

std::shared_ptr<Inode> CreateIDevice(dev_t dev, mode_t mode) {
  return std::make_shared<MemIDevice>(dev, mode);
}

Status<void> InitMemfs() {
  memfs_extent_fd = memfd_create("memfs", MFD_FLAGS);
  if (memfs_extent_fd < 0) {
    memfs_extent_fd = memfd_create("memfs", MFD_FLAGS & ~MFD_EXEC);
    if (memfs_extent_fd < 0) return MakeError(errno);
  }

  int ret = ftruncate(memfs_extent_fd, kMaxMemfdExtent);
  if (ret < 0) return MakeError(errno);

  // Reserve the whole extent region up front, as one PROT_NONE mapping.
  //
  // Three things at once: it proves the region is free, it stops anything else
  // from being placed inside it later (a hole would otherwise be handed to the
  // next mmap that wants an address), and being made before the first clone it
  // exists in every address space -- so a fault in it is always a missing
  // *extent*, never a missing region.
  //
  // Over-allocate by one slot so the base can be rounded up to a 512 GB
  // boundary: page tables are shareable only at that granularity, and a region
  // that straddles a boundary could not be shared without dragging in whatever
  // else sits in the same slot. See docs/shared-page-tables.md.
  constexpr size_t kSlotSize = 512UL << 30;
  constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

  // A preferred base first, so the layout is the same from run to run and can
  // be reasoned about: 96 TiB is above kVirtualAreaMax, above the band the
  // kernel uses for the LibOS image and its heap (~87-89 TiB), and well below
  // the shared libraries and main stack at the top of the address space.
  constexpr uintptr_t kPreferredBase = 0x600000000000;
  void *res = mmap(reinterpret_cast<void *>(kPreferredBase), kMaxMemfdExtent,
                   PROT_NONE, kFlags | MAP_FIXED_NOREPLACE, -1, 0);
  if (res == reinterpret_cast<void *>(kPreferredBase)) {
    memfs_base = kPreferredBase;
  } else {
    // Something is already there. Let the kernel choose, over-allocating by a
    // slot so the base can still be rounded up to a 512 GB boundary.
    if (res != MAP_FAILED) munmap(res, kMaxMemfdExtent);
    res = mmap(nullptr, kMaxMemfdExtent + kSlotSize, PROT_NONE, kFlags, -1, 0);
    if (res == MAP_FAILED) {
      LOG(ERR) << "memfs: could not reserve " << (kMaxMemfdExtent >> 40)
               << " TiB for extents";
      return MakeError(ENOMEM);
    }
    memfs_base = AlignUp(reinterpret_cast<uintptr_t>(res), kSlotSize);
    LOG(INFO) << "memfs: 0x" << std::hex << kPreferredBase
              << " was taken; reserved 0x" << memfs_base << std::dec
              << " instead";
  }

  // A region below kVirtualAreaMax would be inside the guest-addressable
  // range, where a guest could be given an address that the fault handler
  // would then treat as repairable LibOS memory.
  if (memfs_base < kVirtualAreaMax) {
    LOG(ERR) << "memfs: the reserved region landed at 0x" << std::hex
             << memfs_base << ", below kVirtualAreaMax" << std::dec;
    return MakeError(ENOMEM);
  }

  // Hand the region to the fault handler. From here on, a fault anywhere in it
  // is repaired by mapping the enclosing extent at the matching offset --
  // which is correct only because the address and the offset are the same
  // number plus a constant.
  return RegisterManagedSlot("memfs", memfs_base, kMaxMemfdExtent,
                             kMaxSizeBytes, memfs_extent_fd,
                             PROT_READ | PROT_WRITE);
}


std::map<ino_t, size_t> MemInode::traced_inodes_;

void MemFSStartTracer(IDir &root) {
  std::function<void(DirectoryEntry & cur)> fn([&](DirectoryEntry &cur) {
    MemInode *ino = dynamic_cast_guarded<MemInode *>(&cur.get_inode_ref());
    if (ino) ino->RegisterInodeForTracing();
    if (!cur.get_inode_ref().is_dir()) return;
    IDir &dir = static_cast<IDir &>(cur.get_inode_ref());
    dir.ForEach(fn);
  });
  root.ForEach(fn);
}

void MemFSEndTracer() { MemInode::ClearTracedMap(); }

}  // namespace junction::memfs

CEREAL_REGISTER_TYPE(junction::memfs::MemInode);
CEREAL_REGISTER_TYPE(junction::memfs::MemIDir);
CEREAL_REGISTER_TYPE(junction::memfs::MemISoftLink);
CEREAL_REGISTER_TYPE(junction::memfs::MemIDevice);
CEREAL_REGISTER_TYPE(junction::memfs::MemFSFile);
