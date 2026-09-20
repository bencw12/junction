// fifo.cc - named pipes (FIFOs) for memfs
//
// A FIFO is a pipe with a name. The inode owns one channel for its whole life
// and the files opened on it are its ends. What sets it apart from pipe(2) is
// the bookkeeping around open and close: readers and writers come and go, an
// open without O_NONBLOCK waits for the other side to show up, and "the other
// end is closed" means a count reached zero rather than one file going away.
// The channel's closed flags are driven from those counts, so its existing
// EOF, EPIPE, POLLHUP and POLLERR behavior carries over unchanged.

extern "C" {
#include <fcntl.h>
}

#include "junction/fs/memfs/memfs.h"
#include "junction/fs/pipe.h"
#include "junction/limits.h"
#include "junction/snapshot/cereal.h"

namespace junction::memfs {

namespace {

class MemIFifo;

// One open end of a FIFO: the read end, the write end, or (O_RDWR) both.
class FifoFile : public File {
 public:
  FifoFile(std::shared_ptr<MemIFifo> fifo, unsigned int flags, FileMode mode,
           std::shared_ptr<DirectoryEntry> dent) noexcept
      : File(FileType::kNormal, flags, mode, std::move(dent)),
        fifo_(std::move(fifo)) {}
  ~FifoFile() override;

  Status<size_t> Read(std::span<std::byte> buf,
                      [[maybe_unused]] off_t *off) override;
  Status<size_t> Write(std::span<const std::byte> buf,
                       [[maybe_unused]] off_t *off) override;

  [[nodiscard]] bool reads() const { return get_mode() != FileMode::kWrite; }
  [[nodiscard]] bool writes() const { return get_mode() != FileMode::kRead; }

 private:
  friend class cereal::access;

  template <class Archive>
  void save(Archive &ar) const {
    std::shared_ptr<Inode> ino = fifo_;
    ar(ino, get_mode(), get_dent());
    ar(cereal::base_class<File>(this));
  }

  template <class Archive>
  static void load_and_construct(Archive &ar,
                                 cereal::construct<FifoFile> &construct);

  std::shared_ptr<MemIFifo> fifo_;
};

class MemIFifo : public Inode {
 public:
  explicit MemIFifo(mode_t mode, ino_t inum = AllocateInodeNumber())
      : Inode(mode, inum), pipe_(std::make_shared<FifoPipe>(kPipeSize)) {
    // Nothing is open yet: both ends start out closed.
    pipe_->CloseReader();
    pipe_->CloseWriter();
  }
  // Restore: the channel brings its own state, and the files that were open
  // re-register through Adopt().
  MemIFifo(mode_t mode, ino_t inum, std::shared_ptr<FifoPipe> pipe)
      : Inode(mode, inum), pipe_(std::move(pipe)) {}
  ~MemIFifo() override = default;

  Status<std::shared_ptr<File>> Open(
      uint32_t flags, FileMode fmode,
      std::shared_ptr<DirectoryEntry> dent) override;
  Status<void> GetStats(struct stat *buf) const override {
    MemInodeToStats(*this, buf);
    return {};
  }
  Status<void> GetStatFS(struct statfs *buf) const override {
    StatFs(buf);
    return {};
  }

  [[nodiscard]] FifoPipe &pipe() { return *pipe_; }

  // Adopt counts a file whose open already happened (snapshot restore).
  //
  // TODO(snapshot): the save/load paths below (and FifoFile's) compile and
  // mirror MemIDevice/PipeReaderFile, but no snapshot of an open FIFO has been
  // taken and restored yet. Untested claims: readers_/writers_ rebuilt to the
  // saved channel's closed flags; a waiter parked in Open() across a snapshot.
  void Adopt(FifoFile &f) {
    {
      rt::SpinGuard g(lock_);
      AcquireLocked(f.reads(), f.writes());
      open_waiters_.WakeAll();
    }
    AttachPolls(f);
  }

  // Release is the close of one end.
  void Release(FifoFile &f) {
    rt::SpinGuard g(lock_);
    ReleaseLocked(f.reads(), f.writes(), &f.get_poll_source());
  }

  template <class Archive>
  void save(Archive &ar) const {
    ar(get_mode(), get_inum(), pipe_);
    ar(cereal::base_class<Inode>(this));
  }

  template <class Archive>
  static void load_and_construct(Archive &ar,
                                 cereal::construct<MemIFifo> &construct) {
    mode_t mode;
    ino_t inum;
    std::shared_ptr<FifoPipe> pipe;
    ar(mode, inum, pipe);
    construct(mode, inum, std::move(pipe));
    ar(cereal::base_class<Inode>(construct.ptr()));
  }

 private:
  // The first open of an end reopens it on the channel; the last close closes
  // it there. In between, only the counts move.
  void AcquireLocked(bool rd, bool wr) {
    assert(lock_.IsHeld());
    if (rd) {
      reader_opens_++;
      if (readers_++ == 0) pipe_->ReopenReader();
    }
    if (wr) {
      writer_opens_++;
      if (writers_++ == 0) pipe_->ReopenWriter();
    }
  }
  void ReleaseLocked(bool rd, bool wr, PollSource *p) {
    assert(lock_.IsHeld());
    if (rd) {
      assert(readers_ > 0);
      if (--readers_ == 0)
        pipe_->CloseReader(p);
      else if (p)
        pipe_->DetachReadPoll(p);
    }
    if (wr) {
      assert(writers_ > 0);
      if (--writers_ == 0)
        pipe_->CloseWriter(p);
      else if (p)
        pipe_->DetachWritePoll(p);
    }
  }
  void AttachPolls(FifoFile &f) {
    PollSource &ps = f.get_poll_source();
    if (f.reads()) pipe_->AttachReadPoll(&ps);
    if (f.writes()) pipe_->AttachWritePoll(&ps);
  }

  rt::Spin lock_;
  rt::WaitQueue open_waiters_;  // opens waiting for the other side
  unsigned int readers_{0};     // open files that can read
  unsigned int writers_{0};     // open files that can write
  // Every open of each side, monotonic. A waiting open watches this rather
  // than the live count: a partner that opens, writes and closes before the
  // waiter gets to run again has still arrived, and must still release it.
  // (Linux: fifo_open waits on pipe->r_counter / w_counter the same way.)
  uint64_t reader_opens_{0};
  uint64_t writer_opens_{0};
  std::shared_ptr<FifoPipe> pipe_;
};

Status<std::shared_ptr<File>> MemIFifo::Open(
    uint32_t flags, FileMode fmode, std::shared_ptr<DirectoryEntry> dent) {
  const bool rd = fmode != FileMode::kWrite;
  const bool wr = fmode != FileMode::kRead;
  const bool nonblock = (flags & kFlagNonblock) != 0;

  {
    rt::SpinGuard g(lock_);
    // POSIX: a nonblocking open for writing with no reader fails instead of
    // waiting. (A nonblocking open for reading always succeeds.)
    if (wr && !rd && nonblock && readers_ == 0) return MakeError(ENXIO);

    // Count ourselves first so an open on the other side sees us.
    AcquireLocked(rd, wr);
    open_waiters_.WakeAll();

    // Rendezvous: a plain reader waits for a writer and a plain writer waits
    // for a reader. O_RDWR is both at once and never waits; nor does
    // O_NONBLOCK. Satisfied by a partner present now, or by any partner that
    // has opened since we started waiting, even one already gone again.
    if (!nonblock && rd != wr) {
      const uint64_t seen = rd ? writer_opens_ : reader_opens_;
      bool ok = rt::WaitInterruptible(lock_, open_waiters_, [&] {
        if (rd) return writers_ > 0 || writer_opens_ != seen;
        return readers_ > 0 || reader_opens_ != seen;
      });
      if (!ok) {
        ReleaseLocked(rd, wr, nullptr);
        return MakeError(ERESTARTSYS);  // Linux restarts this open
      }
    }
  }

  auto f = std::make_shared<FifoFile>(shared_from_base<MemIFifo>(), flags,
                                      fmode, std::move(dent));
  AttachPolls(*f);
  return f;
}

FifoFile::~FifoFile() { fifo_->Release(*this); }

Status<size_t> FifoFile::Read(std::span<std::byte> buf, off_t *) {
  if (!reads()) return MakeError(EBADF);
  return fifo_->pipe().Read(buf, is_nonblocking());
}

Status<size_t> FifoFile::Write(std::span<const std::byte> buf, off_t *) {
  if (!writes()) return MakeError(EBADF);
  return fifo_->pipe().Write(buf, is_nonblocking());
}

template <class Archive>
void FifoFile::load_and_construct(Archive &ar,
                                  cereal::construct<FifoFile> &construct) {
  std::shared_ptr<Inode> ino;
  FileMode mode;
  std::shared_ptr<DirectoryEntry> dent;
  ar(ino, mode, dent);
  auto fifo = std::static_pointer_cast<MemIFifo>(std::move(ino));
  construct(fifo, 0, mode, std::move(dent));
  ar(cereal::base_class<File>(construct.ptr()));
  fifo->Adopt(*construct.ptr());
}

}  // namespace

std::shared_ptr<Inode> CreateIFifo(mode_t mode) {
  return std::make_shared<MemIFifo>(mode);
}

}  // namespace junction::memfs

CEREAL_REGISTER_TYPE(junction::memfs::MemIFifo);
CEREAL_REGISTER_TYPE(junction::memfs::FifoFile);
