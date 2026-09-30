#pragma once

extern "C" {
#include <base/syscall.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
}

#include <cstddef>
#include <cstdlib>
#include <span>
#include <utility>

#include "junction/base/error.h"
#include "junction/base/io.h"
#include "junction/fs/file.h"

namespace junction {

// Records a memory operation when --trace_libos_mem is on. The enum lives here
// rather than in memtrace.h because memtrace.cc needs ksys.h, and the mapping
// wrappers below need the enum.
enum class MemTraceSource : uint8_t {
  kJunctionInternal,  // Junction's own code, deliberately
  kJunctionLibc,      // Junction's glibc, trapped by seccomp
  kCaladan,           // the runtime's allocators
  kGuestSyscall,      // a guest's mmap/munmap, serviced by Junction
};
void MemTraceMap(MemTraceSource src, const char *op, uintptr_t addr, size_t len,
                 int prot, int flags, int fd, int64_t off, uintptr_t caller,
                 int64_t result);
bool MemTraceEnabled();

// Reports a structural change to memory that other address spaces have already
// inherited. Unlike the trace, this is always on: it costs one relaxed atomic
// load before the first clone, and the bug it catches is silent. See
// CheckFrozenViolation() in memtrace.cc for what counts and why.
void CheckFrozenViolation(const char *op, uintptr_t addr, size_t len);

#define CHECK_FROZEN(op, addr, len) \
  CheckFrozenViolation(op, reinterpret_cast<uintptr_t>(addr), len)

#define TRACE_MEM(source, op, addr, len, prot, flags, fd, off, res)          \
  do {                                                                       \
    if (unlikely(MemTraceEnabled()))                                         \
      MemTraceMap(MemTraceSource::source, op, reinterpret_cast<uintptr_t>(addr), \
                  len, prot, flags, fd, off,                                 \
                  reinterpret_cast<uintptr_t>(__builtin_return_address(0)),  \
                  static_cast<int64_t>(res));                                \
  } while (0)


inline constexpr int kMaxIovLen = 1024;  // Linux's max.

// Touches a string the kernel is about to be given, and returns its address.
//
// LibOS memory is installed in an address space lazily: a core that touches a
// page its address space has never seen faults, and the fault handler maps the
// page in. That works for the LibOS's own accesses. It does not work for the
// kernel's: given a pointer to such a page, copy_from_user() does not deliver a
// fault we could repair, it fails the system call with EFAULT. A path string is
// the case that happens -- with a writable host filesystem an inode, and the
// path it caches, is created at run time by whichever process first names the
// file, and used later by processes in other address spaces ("curl: error while
// loading shared libraries: libz.so.1: cannot open shared object file: Error
// 14", once a package upgrade run by another process had replaced the file).
inline const char *KernelPath(std::string_view path) {
  const volatile char *p = path.data();
  for (size_t off = 0; off < path.size(); off += 4096) (void)p[off];
  (void)p[path.size()];  // the terminator the kernel reads up to
  return path.data();
}

#ifdef WRITEABLE_LINUX_FS
constexpr bool linux_fs_writeable() { return true; }
#else
constexpr bool linux_fs_writeable() { return false; }
#endif

// VDSO syscalls
extern int (*ksys_clock_gettime)(clockid_t clockid, struct timespec *tp);

// Available Linux Kernel System Calls (after seccomp_filter is enabled)
extern "C" {
extern long ksys_start;
extern long ksys_end;
// TODO(girfan): We need to eventually remove ksys_default.
long ksys_default(long arg0, long arg1, long arg2, long arg3, long arg4,
                  long arg5, long sys_num);
intptr_t ksys_mmap(void *addr, size_t length, int prot, int flags, int fd,
                   off_t offset);
intptr_t ksys_mremap(void *oldaddr, size_t oldsz, size_t newsz, int flags,
                     void *new_addr);
int ksys_munmap(void *addr, size_t length);
int ksys_mprotect(void *addr, size_t len, int prot);
long ksys_madvise(void *addr, size_t length, int advice);
int ksys_openat(int fd, const char *pathname, int flags, mode_t mode);
int ksys_close(int fd);
ssize_t ksys_pread(int fd, void *buf, size_t count, off_t offset);
int ksys_tgkill(pid_t tgid, pid_t tid, int sig);
ssize_t ksys_readlinkat(int dirfd, const char *pathname, char *buf,
                        size_t bufsz);

static inline int ksys_open(const char *pathname, int flags, mode_t mode) {
  return ksys_openat(AT_FDCWD, pathname, flags, mode);
}

static inline ssize_t ksys_write(int fd, const void *buf, size_t count) {
  return syscall_write(fd, buf, count);
}

#if 0
ssize_t ksys_readv(int fd, const struct iovec *iov, int iovcnt);

static inline ssize_t ksys_read(int fd, void *buf, size_t count) {
  iovec v = {.iov_base = buf, .iov_len = count};
  return ksys_readv(fd, &v, 1);
}
#endif

static inline ssize_t ksys_pwritev(int fd, const struct iovec *iov, int iovcnt,
                                   off_t offset) {
  iovcnt = std::min(kMaxIovLen, iovcnt);
  return syscall_pwritev2(fd, iov, iovcnt, offset, 0, 0);
}
static inline ssize_t ksys_pwrite(int fd, const void *buf, size_t count,
                                  off_t offset) {
  const struct iovec iov = {.iov_base = (void *)buf, .iov_len = count};
  return ksys_pwritev(fd, &iov, 1, offset);
}
int ksys_newfstatat(int dirfd, const char *pathname, struct stat *statbuf,
                    int flags);
int ksys_getdents64(unsigned int fd, void *dirp, unsigned int count);
void ksys_exit(int status) __attribute__((noreturn));
}

template <class T>
concept SyscallArg = std::is_convertible_v<T, long> || std::is_pointer_v<T>;

template <typename A, typename B, typename C, typename D, typename E,
          typename F>
  requires SyscallArg<A> && SyscallArg<B> && SyscallArg<C> && SyscallArg<D> &&
           SyscallArg<E> && SyscallArg<F>
static __always_inline long ksyscall(int sysnr, A arg1, B arg2, C arg3, D arg4,
                                     E arg5, F arg6) {
  return ksys_default((long)arg1, (long)arg2, (long)arg3, (long)arg4,
                      (long)arg5, (long)arg6, sysnr);
}

template <typename A, typename B, typename C, typename D, typename E>
static __always_inline long ksyscall(int sysnr, A arg1, B arg2, C arg3, D arg4,
                                     E arg5) {
  register long arg6 asm("r9");
  return ksyscall(sysnr, arg1, arg2, arg3, arg4, arg5, arg6);
}

template <typename A, typename B, typename C, typename D>
static __always_inline long ksyscall(int sysnr, A arg1, B arg2, C arg3,
                                     D arg4) {
  register long arg5 asm("r8");
  return ksyscall(sysnr, arg1, arg2, arg3, arg4, arg5);
}

template <typename A, typename B, typename C>
static __always_inline long ksyscall(int sysnr, A arg1, B arg2, C arg3) {
  register long arg4 asm("rcx");
  return ksyscall(sysnr, arg1, arg2, arg3, arg4);
}

template <typename A, typename B>
static __always_inline long ksyscall(int sysnr, A arg1, B arg2) {
  register long arg3 asm("rdx");
  return ksyscall(sysnr, arg1, arg2, arg3);
}

template <typename A>
static __always_inline long ksyscall(int sysnr, A arg1) {
  register long arg2 asm("rsi");
  return ksyscall(sysnr, arg1, arg2);
}

static __always_inline long ksyscall(int sysnr) {
  register long arg1 asm("rdi");
  return ksyscall(sysnr, arg1);
}

// Runs a host filesystem call again for as long as it reports EINTR. On an
// ordinary filesystem these calls are never interrupted, but with the chroot
// on overlayfs the first modification of a file from the lower layer -- an
// open for writing, link, rename, chmod, chown, truncate -- copies it up, and
// the copy gives up with EINTR when a signal is pending. The runtime preempts
// with signals, so under core contention that is routine, and the guest sees
// an error Linux would never give it ("dpkg: unable to make backup link ...:
// Interrupted system call"). Bounded, in case a copy-up can never finish
// between two preemptions.
template <typename F>
static __always_inline long KernelRetryEintr(F &&call) {
  long ret;
  int tries = 0;
  do {
    ret = call();
  } while (unlikely(ret == -EINTR) && ++tries < 4096);
  return ret;
}

// KernelFile provides a wrapper around a Linux FD.
class KernelFile : public VectoredWriter {
 public:
  // Open creates a new file descriptor attached to a file path.
  static Status<KernelFile> Open(std::string_view path, int flags,
                                 FileMode fmode, mode_t mode = 0) {
    int ret = KernelRetryEintr([&] { return ksys_open(KernelPath(path), flags | ToFlags(fmode), mode); });
    if (ret < 0) return MakeError(-ret);
    return KernelFile(ret);
  }

  // Open creates a new file descriptor attached to a file path.
  static Status<KernelFile> Open(const char *path, int flags, FileMode fmode,
                                 mode_t mode = 0) {
    int ret = KernelRetryEintr([&] { return ksys_open(path, flags | ToFlags(fmode), mode); });
    if (ret < 0) return MakeError(-ret);
    return KernelFile(ret);
  }

  static Status<KernelFile> OpenAt(int fd, std::string_view path, int flags,
                                   FileMode fmode, mode_t mode = 0) {
    int ret = KernelRetryEintr([&] { return ksys_openat(fd, KernelPath(path), flags | ToFlags(fmode), mode); });
    if (ret < 0) return MakeError(-ret);
    return KernelFile(ret);
  }

  Status<KernelFile> OpenAt(std::string_view path, int flags, FileMode fmode,
                            mode_t mode = 0) {
    int ret = KernelRetryEintr([&] { return ksys_openat(fd_, KernelPath(path), flags | ToFlags(fmode), mode); });
    if (ret < 0) return MakeError(-ret);
    return KernelFile(ret);
  }

  KernelFile() noexcept = default;
  explicit KernelFile(int fd) noexcept : fd_(fd) {}
  ~KernelFile() {
    if (fd_ >= 0) ksys_close(fd_);
  }

  // disable copy.
  KernelFile(const KernelFile &) = delete;
  KernelFile &operator=(const KernelFile &) = delete;

  // allow move.
  KernelFile(KernelFile &&f) noexcept
      : fd_(std::exchange(f.fd_, -1)), off_(std::exchange(f.off_, 0)) {}
  KernelFile &operator=(KernelFile &&f) noexcept {
    fd_ = std::exchange(f.fd_, -1);
    off_ = std::exchange(f.off_, 0);
    return *this;
  }

  // Read from the file.
  Status<size_t> Read(std::span<std::byte> buf) {
    ssize_t ret = ksys_pread(fd_, buf.data(), buf.size_bytes(), off_);
    if (ret < 0) return MakeError(static_cast<int>(-ret));
    if (ret == 0) return MakeError(EUNEXPECTEDEOF);
    off_ += ret;
    return static_cast<size_t>(ret);
  }

  // Write to the file.
  Status<size_t> Write(std::span<const std::byte> buf) {
    ssize_t ret = ksys_pwrite(fd_, buf.data(), buf.size_bytes(), off_);
    if (ret < 0) return MakeError(static_cast<int>(-ret));
    off_ += ret;
    return static_cast<size_t>(ret);
  }

  // Write to the file.
  Status<size_t> Writev(std::span<const iovec> iov) {
    ssize_t ret = ksys_pwritev(fd_, iov.data(), iov.size(), off_);
    if (ret < 0) return MakeError(static_cast<int>(-ret));
    off_ += ret;
    return static_cast<size_t>(ret);
  }

  // Map a portion of the file.
  Status<void *> MMap(size_t length, int prot, int flags, off_t off) {
    assert(!(flags & (MAP_FIXED | MAP_ANONYMOUS)));
    flags |= MAP_PRIVATE;
    intptr_t ret = ksys_mmap(nullptr, length, prot, flags, fd_, off);
    TRACE_MEM(kJunctionInternal, "mmap-file", 0UL, length, prot, flags, fd_,
              off, ret);
    if (ret < 0) return MakeError(-ret);
    return reinterpret_cast<void *>(ret);
  }

  // Map a portion of the file to a fixed address.
  Status<void> MMapFixed(void *addr, size_t length, int prot, int flags,
                         off_t off) {
    assert(!(flags & MAP_ANONYMOUS));
    flags |= MAP_FIXED | MAP_PRIVATE;
    TRACE_MEM(kJunctionInternal, "mmap-file-fixed", addr, length, prot, flags,
              fd_, off, 0);
    intptr_t ret = ksys_mmap(addr, length, prot, flags, fd_, off);
    if (ret < 0) return MakeError(-ret);
    assert(reinterpret_cast<void *>(ret) == addr);
    return {};
  }

  inline Status<struct stat> StatAt() const {
    struct stat buf;
    int ret =
        ksys_newfstatat(fd_, "", &buf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    if (ret < 0) return MakeError(-ret);
    return buf;
  }

  inline Status<struct stat> StatAt(std::string_view path) {
    struct stat buf;
    int ret = ksys_newfstatat(fd_, KernelPath(path), &buf, AT_SYMLINK_NOFOLLOW);
    if (ret < 0) return MakeError(-ret);
    return buf;
  }

  Status<void> UnlinkAt(std::string_view path, int flags = 0) {
    if constexpr (!linux_fs_writeable()) return MakeError(EACCES);
    int ret = KernelRetryEintr([&] { return ksyscall(__NR_unlinkat, fd_, KernelPath(path), flags); });
    if (ret < 0) return MakeError(-ret);
    return {};
  }

  Status<std::string_view> ReadLinkAt(std::string_view path,
                                      std::span<char> buf) {
    ssize_t wret = ksys_readlinkat(fd_, KernelPath(path), buf.data(), buf.size());
    if (wret < 0) return MakeError(-wret);
    return {{buf.data(), static_cast<size_t>(wret)}};
  }

  Status<void> MkDirAt(std::string_view path, mode_t mode) {
    if constexpr (!linux_fs_writeable()) return MakeError(EACCES);
    int ret = KernelRetryEintr([&] { return ksyscall(__NR_mkdirat, fd_, KernelPath(path), mode); });
    if (ret < 0) return MakeError(-ret);
    return {};
  }

  Status<void> ChModAt(std::string_view path, mode_t mode) {
    if constexpr (!linux_fs_writeable()) return {};
    int ret = KernelRetryEintr([&] { return ksyscall(__NR_fchmodat, fd_, KernelPath(path), mode); });
    if (ret < 0) return MakeError(-ret);
    return {};
  }

  Status<void> ChOwnAt(std::string_view path, uid_t uid, gid_t gid) {
    if constexpr (!linux_fs_writeable()) return {};
    int ret = KernelRetryEintr([&] { return ksyscall(__NR_fchownat, fd_, KernelPath(path), uid, gid,
                       AT_SYMLINK_NOFOLLOW); });
    if (ret < 0) return MakeError(-ret);
    return {};
  }

  Status<void> SymLinkAt(std::string_view target, std::string_view path) {
    if constexpr (!linux_fs_writeable()) return MakeError(EACCES);
    int ret = KernelRetryEintr([&] { return ksyscall(__NR_symlinkat, KernelPath(target), fd_, KernelPath(path)); });
    if (ret < 0) return MakeError(-ret);
    return {};
  }

  static Status<void> RenameAt(KernelFile &olddir, std::string_view oldpath,
                               KernelFile &newdir, std::string_view newpath,
                               bool replace) {
    if constexpr (!linux_fs_writeable()) return MakeError(EACCES);
    int flags = replace ? 0 : RENAME_NOREPLACE;
    int ret = KernelRetryEintr([&] { return ksyscall(__NR_renameat2, olddir.fd_, KernelPath(oldpath), newdir.fd_,
                       KernelPath(newpath), flags); });
    if (ret < 0) return MakeError(-ret);
    return {};
  }

  static Status<void> LinkAt(KernelFile &olddir, std::string_view oldpath,
                             KernelFile &newdir, std::string_view newpath) {
    if constexpr (!linux_fs_writeable()) return MakeError(EACCES);
    int ret = KernelRetryEintr([&] { return ksyscall(__NR_linkat, olddir.fd_, KernelPath(oldpath), newdir.fd_,
                       KernelPath(newpath), 0); });
    if (ret < 0) return MakeError(-ret);
    return {};
  }

  // Seek to a different position in the file.
  void Seek(off_t offset) { off_ = offset; }
  [[nodiscard]] off_t Tell() const { return off_; }

  [[nodiscard]] int GetFd() const { return fd_; }
  void Release() { fd_ = -1; }

 private:
  int fd_{-1};
  off_t off_{0};
};

// Map anonymous memory. MAP_PRIVATE is the default, but a caller that asked
// for MAP_SHARED keeps it: setting both bits does not mean "either one", it
// spells MAP_SHARED_VALIDATE, and the kernel rejects it here.
inline Status<void *> KernelMMap(void *addr, size_t length, int prot,
                                 int flags) {
  flags |= MAP_ANONYMOUS;
  if (!(flags & MAP_SHARED)) flags |= MAP_PRIVATE;
  intptr_t ret = ksys_mmap(addr, length, prot, flags, -1, 0);
  TRACE_MEM(kJunctionInternal, "mmap", addr, length, prot, flags, -1, 0, ret);
  if (ret < 0) return MakeError(-ret);
  return reinterpret_cast<void *>(ret);
}

// Map anonymous memory to a fixed address.
inline Status<void> KernelMMapFixed(void *addr, size_t length, int prot,
                                    int flags) {
  flags |= MAP_ANONYMOUS | MAP_FIXED;
  if (!(flags & MAP_SHARED)) flags |= MAP_PRIVATE;
  intptr_t ret = ksys_mmap(addr, length, prot, flags, -1, 0);
  TRACE_MEM(kJunctionInternal, "mmap", addr, length, prot, flags, -1, 0, ret);
  if (ret < 0) return MakeError(-ret);
  assert(reinterpret_cast<void *>(ret) == addr);
  return {};
}

// Unmap memory.
inline Status<void> KernelMUnmap(void *addr, size_t length) {
  int ret = ksys_munmap(addr, length);
  TRACE_MEM(kJunctionInternal, "munmap", addr, length, 0, 0, -1, 0, ret);
  if (ret < 0) return MakeError(-ret);
  CHECK_FROZEN("munmap", addr, length);
  return {};
}

// Change memory permissions.
inline Status<void> KernelMProtect(void *addr, size_t length, int prot) {
  int ret = ksys_mprotect(addr, length, prot);
  TRACE_MEM(kJunctionInternal, "mprotect", addr, length, prot, 0, -1, 0, ret);
  if (ret < 0) return MakeError(-ret);
  CHECK_FROZEN("mprotect", addr, length);
  return {};
}

// Pass mapping hints.
inline Status<void> KernelMAdvise(void *addr, size_t length, int hint) {
  int ret = ksys_madvise(addr, length, hint);
  TRACE_MEM(kJunctionInternal, "madvise", addr, length, 0, hint, -1, 0, ret);
  if (ret < 0) return MakeError(-ret);
  // MADV_REMOVE is the one hint that propagates: it punches the shared object,
  // so every address space mapping it sees the same result. MADV_DONTNEED only
  // drops the caller's PTEs, and on private memory it discards the contents
  // outright -- which is why it has to be translated, not inherited.
  if (hint == MADV_DONTNEED) CHECK_FROZEN("madvise(DONTNEED)", addr, length);
  return {};
}

inline Status<void *> KernelMRemap(void *old_addr, size_t old_sz,
                                   size_t new_len, int flags,
                                   void *new_addr = nullptr) {
  intptr_t ret = ksys_mremap(old_addr, old_sz, new_len, flags, new_addr);
  TRACE_MEM(kJunctionInternal, "mremap", old_addr, old_sz, 0, flags, -1,
            static_cast<int64_t>(new_len), ret);
  if (ret < 0) return MakeError(-ret);
  CHECK_FROZEN("mremap", old_addr, old_sz);
  return reinterpret_cast<void *>(ret);
}

// A wrapper around a struct stat that prevents zeroing on construction.
struct KernelStatBuf {
  struct stat s;
  KernelStatBuf() noexcept { /* do nothing */
  }
  operator struct stat &() { return s; }
  operator const struct stat &() const { return s; }
  struct stat *ptr() { return &s; }

  [[nodiscard]] bool is_directory() const { return S_ISDIR(s.st_mode); }
  [[nodiscard]] bool is_symlink() const { return S_ISLNK(s.st_mode); }
  [[nodiscard]] bool is_regular() const { return S_ISREG(s.st_mode); }

  [[nodiscard]] dev_t st_dev() const { return s.st_dev; }
};

inline Status<KernelStatBuf> KernelStat(const char *path) {
  Status<KernelStatBuf> res;
  int ret = ksys_newfstatat(AT_FDCWD, path, res->ptr(), 0);
  if (ret < 0) res = MakeError(-ret);
  return res;
}

}  // namespace junction
