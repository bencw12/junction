#pragma once

#include <sys/stat.h>

#include <set>

#include "junction/fs/fs.h"
#include "junction/fs/memfs/memfs.h"
#include "junction/kernel/ksys.h"
#include "junction/snapshot/cereal.h"

namespace junction::linuxfs {

extern KernelFile linux_root_fd;
extern struct statfs linux_statfs;
extern std::set<dev_t> allowed_devs;

__noinline void LinuxFSPanic(std::string_view msg, Error err);

// With a writable host filesystem the file is the host's, and so are its
// timestamps: copy them over whatever the inode remembered. A path that no
// longer resolves (unlinked while open) keeps what it had.
inline void HostTimes(const std::string &path, struct stat *buf) {
  if constexpr (!linux_fs_writeable()) return;
  Status<struct stat> host = linux_root_fd.StatAt(path);
  if (!host) return;
  buf->st_atim = host->st_atim;
  buf->st_mtim = host->st_mtim;
  buf->st_ctim = host->st_ctim;
  buf->st_blocks = host->st_blocks;
  // The link count too: useradd locks /etc/passwd by hard-linking a temporary
  // file to the lock name and checking that the count became 2.
  buf->st_nlink = host->st_nlink;
  // And the owner: the image's files arrive with theirs, chown() is passed
  // through to the host, and the host copy outlives this inode.
  buf->st_uid = host->st_uid;
  buf->st_gid = host->st_gid;
}

class LinuxISoftLink : public memfs::MemISoftLink {
 public:
  LinuxISoftLink(const struct stat &stat, std::string path)
      : MemISoftLink(stat, std::move(path)) {}
  LinuxISoftLink(ino_t ino, std::string path)
      : MemISoftLink(std::move(path), ino) {}
  bool SnapshotPrunable() override { return true; }
};

class LinuxInode : public Inode {
 public:
  LinuxInode(const struct stat &stat, std::string path)
      : Inode(stat.st_mode, stat.st_ino),
        path_(std::move(path)),
        size_(stat.st_size),
        mtime_(stat.st_mtime),
        dev_(stat.st_dev) {
    assert(!is_symlink() && !is_dir());
  }

  // Open a file for this inode.
  Status<std::shared_ptr<File>> Open(
      uint32_t flags, FileMode mode,
      std::shared_ptr<DirectoryEntry> dent) override;

  [[nodiscard]] int fs_kind() const override { return 1; /* kFsLinuxfs */ }
  bool SnapshotPrunable() override { return true; }

  // Get attributes.
  Status<void> GetStats(struct stat *buf) const override {
    InodeToStats(*this, buf);
    buf->st_size = get_size();
    buf->st_nlink = 1;
    buf->st_mtime = mtime_;
    buf->st_dev = dev_;
    HostTimes(path_, buf);
    return {};
  }

  Status<void> GetStatFS(struct statfs *buf) const override {
    *buf = linux_statfs;
    return {};
  }

  [[nodiscard]] off_t get_size() const;
  Status<void> ChangeMode(mode_t new_mode) override {
    if (Status<void> ret = linux_root_fd.ChModAt(path_, new_mode & kModeMask);
        !ret)
      return ret;
    SetMode(new_mode);
    return {};
  }

  Status<void> ChangeOwner(uid_t uid, gid_t gid) override {
    if (Status<void> ret = linux_root_fd.ChOwnAt(path_, uid, gid); !ret)
      return ret;
    return Inode::ChangeOwner(uid, gid);
  }

  [[nodiscard]] std::string_view get_path() const { return path_; }
  [[nodiscard]] Status<void> SetSize(size_t sz) override;

 private:
  friend class LinuxWrIDir;
  std::string path_;
  mutable off_t size_;  // refreshed by get_size()
  const time_t mtime_;
  const dev_t dev_;
};

class LinuxIDir : public memfs::MemIDir {
 public:
  LinuxIDir(Token t, const struct stat &stat, std::string path)
      : MemIDir(t, stat), path_(std::move(path)) {
    assert(is_dir());
  }

  Status<void> ChangeMode(mode_t new_mode) override {
    if (Status<void> ret = linux_root_fd.ChModAt(path_, new_mode & kModeMask);
        !ret)
      return ret;
    SetMode(new_mode);
    return {};
  }

  Status<void> ChangeOwner(uid_t uid, gid_t gid) override {
    if (Status<void> ret = linux_root_fd.ChOwnAt(path_, uid, gid); !ret)
      return ret;
    return Inode::ChangeOwner(uid, gid);
  }

  bool SnapshotPrunable() override { return true; }
  bool SnapshotRecurse() override { return path_ != "/tmp"; }

  // Inode ops
  Status<void> GetStats(struct stat *buf) const override {
    InodeToStats(*this, buf);
    HostTimes(path_, buf);
    return {};
  }

  Status<void> GetStatFS(struct statfs *buf) const override {
    *buf = linux_statfs;
    return {};
  }

 protected:
  // Helper routine to intialize entries_.
  Status<void> FillEntries();
  void DoInitialize() override;

  Status<DirectoryEntry *> AddInode(const struct stat &stat,
                                    std::string abspath,
                                    std::string_view entry_name);

  virtual DirectoryEntry *InstantiateChildDir(const struct stat &buf,
                                              std::string abspath,
                                              std::string name) {
    assert_locked();
    return AddIDirLockedNoCheck<LinuxIDir>(std::move(name), buf,
                                           std::move(abspath));
  }

  Status<KernelFile> GetLinuxDirFD() const {
    return linux_root_fd.OpenAt(path_, O_DIRECTORY, FileMode::kRead);
  }

  inline std::string AppendFileName(std::string_view name) const {
    std::string result;
    result.reserve(path_.size() + 1 + name.size());
    result.append(path_);
    result.append("/");
    result.append(name);
    return result;
  }

  std::string path_;
};

class LinuxWrIDir : public LinuxIDir {
 public:
  LinuxWrIDir(Token t, const struct stat &stat, std::string path)
      : LinuxIDir(t, stat, std::move(path)) {
    assert(is_dir());
  }

  // Directory ops
  Status<void> MkNod(std::string_view name, mode_t mode, dev_t dev) override {
    return MakeError(EACCES);
  }

  Status<void> MkDir(std::string_view name, mode_t mode) override;
  Status<void> Unlink(std::string_view name) override;
  Status<void> RmDir(std::string_view name) override;
  Status<void> SymLink(std::string_view name, std::string_view target) override;
  // Rebuilds the cached host paths of everything looked up under this
  // directory, after the directory itself has moved.
  void RepathChildren();

  Status<void> Rename(IDir &src, std::string_view src_name,
                      std::string_view dst_name, bool replace) override;
  Status<void> Link(std::string_view name, std::shared_ptr<Inode> ino) override;
  Status<std::shared_ptr<File>> Create(std::string_view name, int flags,
                                       mode_t mode, FileMode fmode) override;

 protected:
  DirectoryEntry *InstantiateChildDir(const struct stat &buf,
                                      std::string abspath,
                                      std::string name) override {
    assert_locked();
    return AddIDirLockedNoCheck<LinuxWrIDir>(std::move(name), buf,
                                             std::move(abspath));
  }

 private:
  // Helper routine for renaming.
  Status<void> DoRename(LinuxWrIDir &src, std::string_view src_name,
                        std::string_view dst_name, bool replace);
};

}  // namespace junction::linuxfs
