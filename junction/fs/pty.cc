// pty.cc - pseudo-terminals
//
// /dev/ptmx, /dev/pts/N and /dev/tty. A pty is two byte channels back to back
// -- what the master writes the slave reads, and the other way round -- with a
// line discipline in between, which is the part that makes it a terminal:
// canonical-mode line editing and echo, CR/NL translation in both directions,
// and the control characters that become signals to the foreground process
// group. Programs need one whenever they drive another program "as a user
// would": expect, ssh, script, tmux, Python's pty module. They also need one to
// get isatty() == true, which is what decides whether a shell does job control
// and whether a tool pages, colours or prompts.
//
// What is not here: background-process restrictions (SIGTTIN/SIGTTOU), flow
// control (IXON is accepted and ignored), packet mode, and VMIN/VTIME -- a raw
// read returns as soon as anything is available, as with VMIN=1, VTIME=0.

extern "C" {
#include <sys/ioctl.h>
#include <termios.h>
}

#include <map>
#include <string>

#include "junction/bindings/log.h"
#include "junction/bindings/sync.h"
#include "junction/fs/dev.h"
#include "junction/fs/file.h"
#include "junction/fs/fs.h"
#include "junction/fs/pipe.h"
#include "junction/fs/pty.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/usys.h"

#ifndef TIOCGPTPEER
#define TIOCGPTPEER _IO('T', 0x41)
#endif

namespace junction {

namespace {

// The kernel's termios, which is what TCGETS and TCSETS carry. It is not
// glibc's struct termios, which is larger and laid out differently.
struct k_termios {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[19];
};

// TCGETS2 and friends: the same, with explicit speeds.
struct k_termios2 {
  k_termios t;
  uint32_t c_ispeed;
  uint32_t c_ospeed;
};

inline constexpr unsigned long kTCGETS2 = _IOR('T', 0x2A, k_termios2);
inline constexpr unsigned long kTCSETS2 = _IOW('T', 0x2B, k_termios2);
inline constexpr unsigned long kTCSETSW2 = _IOW('T', 0x2C, k_termios2);
inline constexpr unsigned long kTCSETSF2 = _IOW('T', 0x2D, k_termios2);

inline constexpr size_t kPtyBufSize = 64 * 1024;
inline constexpr size_t kMaxCanonLine = 4096;
inline constexpr dev_t kPtsMajor = 136;
inline constexpr char kEofMark = 0x04;  // in-band end-of-file, canonical mode

class Pty;

rt::Spin pty_lock;                              // protects the three below
std::map<int, std::weak_ptr<Pty>> ptys;         // by index
std::map<pid_t, std::weak_ptr<Pty>> ctty;       // controlling tty, by session
std::shared_ptr<IDir> pts_dir;                  // /dev/pts

mode_t PtsNodeMode() {
  return kTypeCharacter | S_IRUSR | S_IWUSR | S_IWGRP;
}

void SignalPgrp(pid_t pgid, int sig) {
  if (pgid > 0) usys_kill(-pgid, sig);
}

// The state the two ends share.
class Pty : public std::enable_shared_from_this<Pty> {
 public:
  explicit Pty(int idx) : idx_(idx), in_(kPtyBufSize), out_(kPtyBufSize) {
    std::memset(&tio_, 0, sizeof(tio_));
    tio_.c_iflag = ICRNL | IXON;
    tio_.c_oflag = OPOST | ONLCR;
    tio_.c_cflag = B38400 | CS8 | CREAD;
    tio_.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    tio_.c_cc[VINTR] = 0x03;
    tio_.c_cc[VQUIT] = 0x1c;
    tio_.c_cc[VERASE] = 0x7f;
    tio_.c_cc[VKILL] = 0x15;
    tio_.c_cc[VEOF] = 0x04;
    tio_.c_cc[VMIN] = 1;
    tio_.c_cc[VSTART] = 0x11;
    tio_.c_cc[VSTOP] = 0x13;
    tio_.c_cc[VSUSP] = 0x1a;
    tio_.c_cc[VREPRINT] = 0x12;
    tio_.c_cc[VDISCARD] = 0x0f;
    tio_.c_cc[VWERASE] = 0x17;
    tio_.c_cc[VLNEXT] = 0x16;
    ws_ = {24, 80, 0, 0};
  }

  [[nodiscard]] int index() const { return idx_; }
  FifoPipe &in() { return in_; }    // master -> slave
  FifoPipe &out() { return out_; }  // slave -> master

  // Input from the master, through the line discipline, to the slave.
  Status<size_t> MasterWrite(std::span<const std::byte> buf, bool nonblocking) {
    std::string to_slave, echo;
    int sig = 0;
    pid_t pgid;
    {
      rt::SpinGuard g(lock_);
      pgid = fg_pgid_;
      for (std::byte b : buf) {
        int s = Input(static_cast<char>(b), to_slave, echo);
        if (s) sig = s;
      }
    }
    // The echo is best effort: a master that is not reading must not stall
    // its own writes.
    if (!echo.empty())
      out_.Write(std::as_bytes(std::span{echo.data(), echo.size()}), true);
    if (!to_slave.empty()) {
      std::span<const std::byte> data =
          std::as_bytes(std::span{to_slave.data(), to_slave.size()});
      while (!data.empty()) {
        Status<size_t> ret = in_.Write(data, nonblocking);
        if (!ret) {
          if (ret.error().code() == EAGAIN) break;  // drop, like a full tty
          return MakeError(ret);
        }
        data = data.subspan(*ret);
      }
    }
    if (sig) SignalPgrp(pgid, sig);
    return buf.size();
  }

  // Output from the slave to the master, through OPOST.
  Status<size_t> SlaveWrite(std::span<const std::byte> buf, bool nonblocking) {
    bool onlcr;
    {
      rt::SpinGuard g(lock_);
      onlcr = (tio_.c_oflag & OPOST) && (tio_.c_oflag & ONLCR);
    }
    if (!onlcr) return WriteAll(out_, buf, nonblocking, buf.size());
    std::string cooked;
    cooked.reserve(buf.size() + 16);
    for (std::byte b : buf) {
      if (static_cast<char>(b) == '\n') cooked.push_back('\r');
      cooked.push_back(static_cast<char>(b));
    }
    return WriteAll(out_,
                    std::as_bytes(std::span{cooked.data(), cooked.size()}),
                    nonblocking, buf.size());
  }

  // What the slave reads. Canonical mode hands over one line per call; the
  // end-of-file character travels in band and ends the read it belongs to.
  Status<size_t> SlaveRead(std::span<std::byte> buf, bool nonblocking) {
    bool canon;
    {
      rt::SpinGuard g(lock_);
      canon = (tio_.c_lflag & ICANON) != 0;
    }
    if (!canon || buf.empty()) return in_.Read(buf, nonblocking);

    Status<size_t> peeked = in_.Read(buf, nonblocking, true);
    if (!peeked || *peeked == 0) return peeked;
    size_t take = *peeked, give = *peeked;
    for (size_t i = 0; i < *peeked; i++) {
      char c = static_cast<char>(buf[i]);
      if (c == '\n') {
        take = give = i + 1;
        break;
      }
      if (c == kEofMark) {
        take = i + 1;
        give = i;
        break;
      }
    }
    Status<size_t> got = in_.Read(buf.subspan(0, take), true);
    if (!got) return got;
    return std::min(give, *got);
  }

  Status<long> Ioctl(unsigned long request, char *argp, bool *handled);

  void SlaveOpened() {
    rt::SpinGuard g(lock_);
    if (slave_opens_++ == 0) out_.ReopenWriter();
  }
  void SlaveClosed() {
    bool last;
    {
      rt::SpinGuard g(lock_);
      last = --slave_opens_ == 0;
    }
    // With no slave left the master reads end-of-file, which is how the
    // program driving the terminal learns that the session is over.
    if (last) out_.CloseWriter();
  }
  void MasterClosed() {
    pid_t pgid;
    {
      rt::SpinGuard g(lock_);
      pgid = fg_pgid_;
    }
    in_.CloseWriter();
    out_.CloseReader();
    SignalPgrp(pgid, SIGHUP);
    SignalPgrp(pgid, SIGCONT);
  }

  [[nodiscard]] bool locked() {
    rt::SpinGuard g(lock_);
    return locked_;
  }

  void MakeControlling() {
    Process &p = myproc();
    rt::SpinGuard g(lock_);
    sid_ = p.get_sid();
    fg_pgid_ = p.get_pgid();
  }

 private:
  static Status<size_t> WriteAll(FifoPipe &ch, std::span<const std::byte> data,
                                 bool nonblocking, size_t report) {
    bool any = false;
    while (!data.empty()) {
      Status<size_t> ret = ch.Write(data, nonblocking);
      if (!ret) {
        if (any && ret.error().code() == EAGAIN) break;
        // Nobody holds the master any more: that is a hung-up terminal.
        if (ret.error().code() == EPIPE) return MakeError(EIO);
        return MakeError(ret);
      }
      any = true;
      data = data.subspan(*ret);
    }
    return report;
  }

  void EchoChar(char c, std::string &echo) const {
    if (!(tio_.c_lflag & ECHO)) return;
    unsigned char u = static_cast<unsigned char>(c);
    if (c == '\n') {
      echo += "\r\n";
    } else if (u < 0x20 && c != '\t' && (tio_.c_lflag & ECHOCTL)) {
      echo.push_back('^');
      echo.push_back(static_cast<char>(u + 0x40));
    } else {
      echo.push_back(c);
    }
  }

  // One input character through the line discipline. Returns a signal to send
  // to the foreground process group, or zero. Called with the lock held.
  int Input(char c, std::string &to_slave, std::string &echo) {
    const tcflag_t ifl = tio_.c_iflag, lfl = tio_.c_lflag;
    if (c == '\r') {
      if (ifl & IGNCR) return 0;
      if (ifl & ICRNL) c = '\n';
    } else if (c == '\n' && (ifl & INLCR)) {
      c = '\r';
    }

    if (lfl & ISIG) {
      int sig = 0;
      if (c == static_cast<char>(tio_.c_cc[VINTR]) && c) sig = SIGINT;
      if (c == static_cast<char>(tio_.c_cc[VQUIT]) && c) sig = SIGQUIT;
      if (c == static_cast<char>(tio_.c_cc[VSUSP]) && c) sig = SIGTSTP;
      if (sig) {
        EchoChar(c, echo);
        if (!(lfl & NOFLSH)) line_.clear();
        return sig;
      }
    }

    if ((ifl & IXON) && (c == static_cast<char>(tio_.c_cc[VSTART]) ||
                         c == static_cast<char>(tio_.c_cc[VSTOP])))
      return 0;

    if (!(lfl & ICANON)) {
      to_slave.push_back(c);
      EchoChar(c, echo);
      return 0;
    }

    if (c == static_cast<char>(tio_.c_cc[VERASE]) || c == '\b') {
      if (!line_.empty()) {
        line_.pop_back();
        if ((lfl & ECHO) && (lfl & ECHOE)) echo += "\b \b";
      }
      return 0;
    }
    if (c == static_cast<char>(tio_.c_cc[VKILL])) {
      if ((lfl & ECHO) && (lfl & ECHOK))
        for (size_t i = 0; i < line_.size(); i++) echo += "\b \b";
      line_.clear();
      return 0;
    }
    if (c == static_cast<char>(tio_.c_cc[VEOF])) {
      // Ends the read: with a partial line, that line without a newline; with
      // none, a read of zero bytes, which is end-of-file.
      to_slave += line_;
      to_slave.push_back(kEofMark);
      line_.clear();
      return 0;
    }
    EchoChar(c, echo);
    if (c == '\n' || (c && (c == static_cast<char>(tio_.c_cc[VEOL]) ||
                            c == static_cast<char>(tio_.c_cc[VEOL2])))) {
      line_.push_back(c);
      to_slave += line_;
      line_.clear();
      return 0;
    }
    if (line_.size() < kMaxCanonLine) line_.push_back(c);
    return 0;
  }

  const int idx_;
  rt::Spin lock_;
  k_termios tio_;
  struct winsize ws_;
  pid_t fg_pgid_{0};
  pid_t sid_{0};
  std::string line_;  // the line being edited, canonical mode
  int slave_opens_{0};
  bool locked_{true};
  FifoPipe in_;
  FifoPipe out_;
};

Status<long> Pty::Ioctl(unsigned long request, char *argp, bool *handled) {
  *handled = true;
  switch (request) {
    case TCGETS: {
      rt::SpinGuard g(lock_);
      std::memcpy(argp, &tio_, sizeof(tio_));
      return 0;
    }
    case kTCGETS2: {
      k_termios2 t2;
      rt::SpinGuard g(lock_);
      t2.t = tio_;
      t2.c_ispeed = t2.c_ospeed = 38400;
      std::memcpy(argp, &t2, sizeof(t2));
      return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case kTCSETS2:
    case kTCSETSW2:
    case kTCSETSF2: {
      std::string flush;
      {
        rt::SpinGuard g(lock_);
        bool was_canon = (tio_.c_lflag & ICANON) != 0;
        std::memcpy(&tio_, argp, sizeof(tio_));
        // Leaving canonical mode hands over what was being edited.
        if (was_canon && !(tio_.c_lflag & ICANON)) flush.swap(line_);
      }
      if (!flush.empty())
        in_.Write(std::as_bytes(std::span{flush.data(), flush.size()}), true);
      return 0;
    }
    case TIOCGWINSZ: {
      rt::SpinGuard g(lock_);
      std::memcpy(argp, &ws_, sizeof(ws_));
      return 0;
    }
    case TIOCSWINSZ: {
      pid_t pgid = 0;
      {
        rt::SpinGuard g(lock_);
        if (std::memcmp(&ws_, argp, sizeof(ws_)) != 0) pgid = fg_pgid_;
        std::memcpy(&ws_, argp, sizeof(ws_));
      }
      SignalPgrp(pgid, SIGWINCH);
      return 0;
    }
    case TIOCSCTTY: {
      MakeControlling();
      rt::SpinGuard g(pty_lock);
      ctty[myproc().get_sid()] = weak_from_this();
      return 0;
    }
    case TIOCNOTTY: {
      rt::SpinGuard g(pty_lock);
      ctty.erase(myproc().get_sid());
      return 0;
    }
    case TIOCGPGRP: {
      rt::SpinGuard g(lock_);
      *reinterpret_cast<pid_t *>(argp) = fg_pgid_ ? fg_pgid_ : myproc().get_pgid();
      return 0;
    }
    case TIOCSPGRP: {
      rt::SpinGuard g(lock_);
      fg_pgid_ = *reinterpret_cast<pid_t *>(argp);
      return 0;
    }
    case TIOCGSID: {
      rt::SpinGuard g(lock_);
      *reinterpret_cast<pid_t *>(argp) = sid_ ? sid_ : myproc().get_sid();
      return 0;
    }
    case TIOCGPTN:
      *reinterpret_cast<unsigned int *>(argp) = static_cast<unsigned int>(idx_);
      return 0;
    case TIOCSPTLCK: {
      rt::SpinGuard g(lock_);
      locked_ = *reinterpret_cast<int *>(argp) != 0;
      return 0;
    }
    case TIOCGPTLCK: {
      rt::SpinGuard g(lock_);
      *reinterpret_cast<int *>(argp) = locked_ ? 1 : 0;
      return 0;
    }
    case TCFLSH: {
      rt::SpinGuard g(lock_);
      line_.clear();
      return 0;
    }
    case TCSBRK:
    case TCSBRKP:
    case TCXONC:
    case TIOCEXCL:
    case TIOCNXCL:
    case TIOCSETD:
      return 0;
    case TIOCGETD:
      *reinterpret_cast<int *>(argp) = 0;  // N_TTY
      return 0;
    case TIOCOUTQ:
      *reinterpret_cast<int *>(argp) = 0;
      return 0;
    default:
      *handled = false;
      return 0;
  }
}

class PtySlaveFile : public File {
 public:
  PtySlaveFile(std::shared_ptr<Pty> pty, unsigned int flags, FileMode mode,
               std::shared_ptr<DirectoryEntry> dent)
      : File(FileType::kSpecial, flags, mode, std::move(dent)),
        pty_(std::move(pty)) {
    pty_->SlaveOpened();
    pty_->in().AttachReadPoll(&get_poll_source());
    pty_->out().AttachWritePoll(&get_poll_source());
  }
  ~PtySlaveFile() override {
    pty_->in().DetachReadPoll(&get_poll_source());
    pty_->out().DetachWritePoll(&get_poll_source());
    pty_->SlaveClosed();
  }

  Status<size_t> Read(std::span<std::byte> buf,
                      [[maybe_unused]] off_t *off) override {
    return pty_->SlaveRead(buf, is_nonblocking());
  }
  Status<size_t> Write(std::span<const std::byte> buf,
                       [[maybe_unused]] off_t *off) override {
    return pty_->SlaveWrite(buf, is_nonblocking());
  }

 private:
  Status<Status<long>> OnIoctl(unsigned long request, char *argp) override {
    bool handled;
    Status<long> ret = pty_->Ioctl(request, argp, &handled);
    if (!handled) return MakeError(ENOTTY);
    return ret;
  }
  [[nodiscard]] Status<size_t> get_input_bytes() const override {
    return pty_->in().get_readable_bytes();
  }

  std::shared_ptr<Pty> pty_;
};

class PtyMasterFile : public File {
 public:
  PtyMasterFile(std::shared_ptr<Pty> pty, unsigned int flags, FileMode mode,
                std::shared_ptr<DirectoryEntry> dent)
      : File(FileType::kSpecial, flags, mode, std::move(dent)),
        pty_(std::move(pty)) {
    pty_->out().AttachReadPoll(&get_poll_source());
    pty_->in().AttachWritePoll(&get_poll_source());
  }
  ~PtyMasterFile() override {
    pty_->out().DetachReadPoll(&get_poll_source());
    pty_->in().DetachWritePoll(&get_poll_source());
    pty_->MasterClosed();
    std::string name = std::to_string(pty_->index());
    {
      rt::SpinGuard g(pty_lock);
      ptys.erase(pty_->index());
    }
    if (pts_dir) (void)pts_dir->Unlink(name);
  }

  Status<size_t> Read(std::span<std::byte> buf,
                      [[maybe_unused]] off_t *off) override {
    Status<size_t> ret = pty_->out().Read(buf, is_nonblocking());
    // Every slave gone: Linux says EIO here rather than end-of-file, and
    // programs that drive a pty treat either as the end of the session.
    if (ret && *ret == 0 && !buf.empty()) return MakeError(EIO);
    return ret;
  }
  Status<size_t> Write(std::span<const std::byte> buf,
                       [[maybe_unused]] off_t *off) override {
    return pty_->MasterWrite(buf, is_nonblocking());
  }

 private:
  Status<Status<long>> OnIoctl(unsigned long request, char *argp) override {
    if (request == TIOCGPTPEER) {
      Status<std::shared_ptr<File>> f = OpenSlave(
          pty_, static_cast<unsigned int>(reinterpret_cast<uintptr_t>(argp)));
      if (!f) return Status<long>(MakeError(f));
      unsigned int fl = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(argp));
      return Status<long>(myproc().get_file_table().Insert(
          std::move(*f), (fl & kFlagCloseExec) > 0));
    }
    bool handled;
    Status<long> ret = pty_->Ioctl(request, argp, &handled);
    if (!handled) return MakeError(ENOTTY);
    return ret;
  }
  [[nodiscard]] Status<size_t> get_input_bytes() const override {
    return pty_->out().get_readable_bytes();
  }

  static Status<std::shared_ptr<File>> OpenSlave(std::shared_ptr<Pty> pty,
                                                 unsigned int flags);

  std::shared_ptr<Pty> pty_;
};

Status<std::shared_ptr<File>> MakeSlave(std::shared_ptr<Pty> pty,
                                        unsigned int flags, FileMode mode,
                                        std::shared_ptr<DirectoryEntry> dent) {
  // A session leader with no controlling terminal that opens one without
  // O_NOCTTY acquires it. Python's pty.fork() relies on this.
  if (!(flags & O_NOCTTY) && myproc().is_session_leader()) {
    bool has;
    {
      rt::SpinGuard g(pty_lock);
      auto it = ctty.find(myproc().get_sid());
      has = it != ctty.end() && !it->second.expired();
      if (!has) ctty[myproc().get_sid()] = pty;
    }
    if (!has) pty->MakeControlling();
  }
  return std::make_shared<PtySlaveFile>(std::move(pty), flags, mode,
                                        std::move(dent));
}

Status<std::shared_ptr<File>> PtyMasterFile::OpenSlave(std::shared_ptr<Pty> pty,
                                                       unsigned int flags) {
  if (!pts_dir) return MakeError(ENODEV);
  Status<std::shared_ptr<DirectoryEntry>> dent =
      pts_dir->LookupDent(std::to_string(pty->index()));
  if (!dent) return MakeError(dent);
  return MakeSlave(std::move(pty), flags, FileMode::kReadWrite,
                   std::move(*dent));
}

}  // namespace

Status<void> SetupPtys(IDir &dev) {
  mode_t mode = kTypeCharacter | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                S_IROTH | S_IWOTH;
  if (Status<void> ret = dev.MkNod("ptmx", mode, MakeDevice(5, 2)); !ret)
    return ret;
  if (Status<void> ret = dev.MkNod("tty", mode, MakeDevice(5, 0)); !ret)
    return ret;
  pts_dir = memfs::MkFolder(dev, "pts");
  return pts_dir->MkNod("ptmx", mode, MakeDevice(5, 2));
}

bool IsPtyDevice(dev_t dev) {
  return dev == MakeDevice(5, 2) || dev == MakeDevice(5, 0) ||
         DeviceMajor(dev) == kPtsMajor;
}

Status<std::shared_ptr<File>> PtyOpen(std::shared_ptr<DirectoryEntry> dent,
                                      dev_t dev, unsigned int flags,
                                      FileMode mode) {
  if (dev == MakeDevice(5, 2)) {  // /dev/ptmx: a new pair
    if (!pts_dir) return MakeError(ENODEV);
    std::shared_ptr<Pty> pty;
    {
      rt::SpinGuard g(pty_lock);
      int idx = 0;
      while (ptys.count(idx)) idx++;
      pty = std::make_shared<Pty>(idx);
      ptys[idx] = pty;
    }
    Status<void> ret = pts_dir->MkNod(std::to_string(pty->index()),
                                      PtsNodeMode(),
                                      MakeDevice(kPtsMajor, pty->index()));
    if (!ret) {
      rt::SpinGuard g(pty_lock);
      ptys.erase(pty->index());
      return MakeError(ret);
    }
    return std::make_shared<PtyMasterFile>(std::move(pty), flags, mode,
                                           std::move(dent));
  }

  std::shared_ptr<Pty> pty;
  {
    rt::SpinGuard g(pty_lock);
    if (dev == MakeDevice(5, 0)) {  // /dev/tty: the controlling terminal
      auto it = ctty.find(myproc().get_sid());
      if (it != ctty.end()) pty = it->second.lock();
      if (!pty) return MakeError(ENXIO);
    } else {
      auto it = ptys.find(static_cast<int>(DeviceMinor(dev)));
      if (it != ptys.end()) pty = it->second.lock();
      if (!pty) return MakeError(EIO);
    }
  }
  if (dev != MakeDevice(5, 0) && pty->locked()) return MakeError(EIO);
  return MakeSlave(std::move(pty), flags | (dev == MakeDevice(5, 0) ? O_NOCTTY : 0),
                   mode, std::move(dent));
}

}  // namespace junction
