// poll.cc - support for poll(), select(), and epoll()

// glibc uses a very expensive way to check bounds in FD sets, so disable it.
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0

#include <memory>

#include "junction/base/compiler.h"
#include "junction/base/intrusive.h"
#include "junction/fs/file.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/usys.h"
#include "junction/snapshot/cereal.h"

namespace junction {

namespace {

constexpr unsigned int kPollInval = POLLNVAL;
constexpr unsigned int kEPollEdgeTriggered = EPOLLET;
constexpr unsigned int kEPollOneShot = EPOLLONESHOT;
constexpr unsigned int kEPollExclusive = EPOLLEXCLUSIVE;

int DoPoll(pollfd *fds, nfds_t nfds, std::optional<Duration> timeout,
           std::optional<k_sigset_t> mask = std::nullopt) {
  int nevents = 0;

  // Check each file; if at least one has events, no need to block.
  FileTable &ftbl = myproc().get_file_table();
  for (nfds_t i = 0; i < nfds; i++) {
    // A negative fd marks a slot to skip: no events, and not an error. Arrays
    // with unused slots are common -- ssh-keyscan polls 256 entries, one in
    // use, and took "255 ready" to mean its connection had failed.
    if (fds[i].fd < 0) {
      fds[i].revents = 0;
      continue;
    }
    File *f = ftbl.Get(fds[i].fd);
    if (unlikely(!f)) {
      fds[i].revents = static_cast<short>(kPollInval);
      nevents++;
      continue;
    }

    PollSource &src = f->get_poll_source();
    short events = static_cast<short>(src.get_events());
    fds[i].revents = events & (fds[i].events | kPollErr | kPollHUp);
    if (fds[i].revents != 0) nevents++;
  }

  // Fast path: Return without blocking.
  if (nevents > 0 || (timeout && timeout->IsZero())) return nevents;

  // Otherwise, init state to block on the FDs and timeout.
  rt::Spin lock;
  rt::ThreadWaker waker;
  rt::WakeOnTimeout timed_out(lock, waker, timeout);
  SigMaskGuard sig(mask);

  // Pack args to avoid heap allocations.
  struct {
    rt::Spin &lock;
    rt::ThreadWaker &waker;
    int &nevents;
  } args = {lock, waker, nevents};

  // The triggers below fire in the *waker's* context -- whichever thread
  // produced the event, which may belong to another process and so be bound
  // to another address space, where this process's guest memory does not
  // exist. So a trigger must not touch @fds. Each records into this LibOS-side
  // copy instead (LibOS memory is the same in every address space), and the
  // results are written back to the guest below, by this thread, in its own
  // address space. Same shape as select's select_fd.
  std::vector<short> revents(nfds);
  for (nfds_t i = 0; i < nfds; i++) revents[i] = fds[i].revents;

  // Setup a trigger for each file.
  std::vector<Poller> triggers;
  triggers.reserve(nfds);
  for (nfds_t i = 0; i < nfds; i++) {
    triggers.emplace_back([&args, &rev = revents[i],
                           events = fds[i].events](unsigned int pev) {
      int delta = rev > 0 ? -1 : 0;
      rev = static_cast<short>(pev) & (events | kPollErr | kPollHUp);
      delta += rev > 0 ? 1 : 0;
      if (delta != 0) {
        rt::SpinGuard g(args.lock);
        args.nevents += delta;
        if (args.nevents > 0) args.waker.Wake();
      }
    });
    if (fds[i].fd < 0) continue;  // skipped slot: its trigger stays detached
    File *f = ftbl.Get(fds[i].fd);
    assert(f != nullptr);
    PollSource &src = f->get_poll_source();
    src.Attach(triggers.back());
  }

  bool signaled;

  while (true) {
    // Block until an event has triggered.
    {
      rt::SpinGuard g(lock);
      signaled = !rt::WaitInterruptible(lock, waker, [&nevents, &timed_out] {
        return nevents > 0 || timed_out;
      });
    }

    for (auto &p : triggers) p.Detach();

    // There's a tiny chance events will get cleared, causing zero @nevents.
    if (likely(nevents > 0 || timed_out || signaled)) break;

    for (nfds_t i = 0; i < nfds; i++) {
      if (fds[i].fd < 0) continue;
      File *f = ftbl.Get(fds[i].fd);
      assert(f != nullptr);
      PollSource &src = f->get_poll_source();
      src.Attach(triggers[i]);
    }
  }

  // Back in this thread's own address space: publish the results.
  for (nfds_t i = 0; i < nfds; i++) fds[i].revents = revents[i];

  if (nevents == 0 && signaled) {
    // Never restarted once a signal handler has run -- Linux exempts
    // select/pselect/poll/ppoll/epoll_wait from SA_RESTART (signal(7)), and
    // programs depend on the EINTR: GNU make's jobserver wait is exactly
    // "pselect until SIGCHLD, then reap", and an SA_RESTART handler (glibc's
    // signal() default) put it straight back to sleep. ERESTARTNOHAND is
    // that rule: restart transparently only if no handler ran. With a timeout
    // the remaining time would need adjusting, so plain EINTR.
    return timeout ? -EINTR : -ERESTARTNOHAND;
  }
  return nevents;
}

struct select_fd {
  int fd;
  short events;
  short revents;
  Poller p;
};

constexpr short kSelectIn = (kPollIn | kPollHUp | kPollErr);
constexpr short kSelectOut = (kPollOut | kPollErr);
constexpr short kSelectExcept = kPollPrio;

std::vector<select_fd> DecodeSelectFDs(int nfds, fd_set *readfds,
                                       fd_set *writefds, fd_set *exceptfds) {
  std::vector<select_fd> sfds;
  sfds.reserve(nfds);

  for (int i = 0; i < nfds; i++) {
    short events = 0;

    if (readfds && FD_ISSET(i, readfds)) {
      events |= kSelectIn;
    }
    if (writefds && FD_ISSET(i, writefds)) {
      events |= kSelectOut;
    }
    if (exceptfds && FD_ISSET(i, exceptfds)) {
      events |= kSelectExcept;
    }

    if (!events) continue;
    sfds.push_back(select_fd{.fd = i, .events = events, .revents = 0, .p = {}});
  }

  return sfds;
}

// The sets are rewritten here and only here -- on success. On an error they
// have to come back as they went in: kwsys (cmake's process runner) waits with
// "while (select(n, &set, ...) < 0 && errno == EINTR) {}", never rebuilding
// the set, and every SIGCHLD used to leave it waiting on nothing, forever.
int EncodeSelectFDs(const std::vector<select_fd> &sfds, fd_set *readfds,
                    fd_set *writefds, fd_set *exceptfds) {
  int count = 0;

  for (const auto &sfd : sfds) {
    // (These masks overlap; a full match is what says the set was given.)
    if ((sfd.events & kSelectIn) == kSelectIn) FD_CLR(sfd.fd, readfds);
    if ((sfd.events & kSelectOut) == kSelectOut) FD_CLR(sfd.fd, writefds);
    if ((sfd.events & kSelectExcept) == kSelectExcept)
      FD_CLR(sfd.fd, exceptfds);
  }

  for (const auto &sfd : sfds) {
    if ((sfd.events & kSelectIn) == kSelectIn &&
        (sfd.revents & kSelectIn) != 0) {
      FD_SET(sfd.fd, readfds);
      count++;
    }
    if ((sfd.events & kSelectOut) == kSelectOut &&
        (sfd.revents & kSelectOut) != 0) {
      FD_SET(sfd.fd, writefds);
      count++;
    }
    if ((sfd.events & kSelectExcept) == kSelectExcept &&
        (sfd.revents & kSelectExcept) != 0) {
      FD_SET(sfd.fd, exceptfds);
      count++;
    }
  }

  return count;
}

std::pair<int, Duration> DoSelect(
    int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
    std::optional<Duration> timeout,
    std::optional<k_sigset_t> mask = std::nullopt) {
  // Check if maximum number of FDs has been exceeded.
  if (unlikely(nfds > FD_SETSIZE)) return std::make_pair(-EINVAL, Duration(0));

  // Decode the events into a more convenient format.
  std::vector<select_fd> sfds =
      DecodeSelectFDs(nfds, readfds, writefds, exceptfds);

  // Check whether events are pending before blocking.
  FileTable &ftbl = myproc().get_file_table();
  int nevents = 0;
  for (auto &sfd : sfds) {
    File *f = ftbl.Get(sfd.fd);
    if (unlikely(!f)) return std::make_pair(-EBADF, Duration(0));
    PollSource &src = f->get_poll_source();
    short pev = static_cast<short>(src.get_events());
    if ((pev & sfd.events) != 0) {
      sfd.revents = pev & sfd.events;
      nevents++;
    }
  }

  // Fast path: Return without blocking.
  if (nevents > 0) {
    int ret = EncodeSelectFDs(sfds, readfds, writefds, exceptfds);
    Duration d = timeout.value_or(Duration(0));
    return std::make_pair(ret, d);
  }
  if (timeout && timeout->IsZero()) {
    EncodeSelectFDs(sfds, readfds, writefds, exceptfds);  // nothing is ready
    return std::make_pair(0, Duration(0));
  }

  // Otherwise, init state to block on the FDs and timeout.
  rt::Spin lock;
  rt::ThreadWaker waker;
  rt::WakeOnTimeout timed_out(lock, waker, timeout);
  SigMaskGuard sig(mask);

  // Pack args to avoid heap allocations.
  struct {
    rt::Spin &lock;
    rt::ThreadWaker &waker;
    int &nevents;
  } args = {lock, waker, nevents};

  // Setup a trigger for each file.
  for (auto &sfd : sfds) {
    sfd.p = Poller([&args, &entry = sfd](unsigned int pev) {
      int delta = entry.revents > 0 ? -1 : 0;
      entry.revents = (static_cast<short>(pev) & entry.events);
      delta += entry.revents > 0 ? 1 : 0;
      if (delta != 0) {
        rt::SpinGuard g(args.lock);
        args.nevents += delta;
        if (args.nevents > 0) args.waker.Wake();
      }
    });
    File *f = ftbl.Get(sfd.fd);
    assert(f != nullptr);
    PollSource &src = f->get_poll_source();
    src.Attach(sfd.p);
  }

  bool signaled;
  while (true) {
    // Block until an event has triggered.
    {
      rt::SpinGuard g(lock);
      signaled = !rt::WaitInterruptible(lock, waker, [&nevents, &timed_out] {
        return nevents > 0 || timed_out;
      });
    }

    for (auto &sfd : sfds) sfd.p.Detach();

    // There's a tiny chance events will get cleared, causing zero @nevents.
    if (likely(nevents > 0 || timed_out || signaled)) break;

    for (auto &sfd : sfds) {
      File *f = ftbl.Get(sfd.fd);
      assert(f != nullptr);
      PollSource &src = f->get_poll_source();
      src.Attach(sfd.p);
    }
  }

  Duration d(0);
  if (timeout) d = timed_out.TimeLeft();
  if (nevents == 0 && signaled)
    return std::make_pair(timeout ? -EINTR : -ERESTARTNOHAND, d);
  int ret = EncodeSelectFDs(sfds, readfds, writefds, exceptfds);
  return std::make_pair(ret, d);
}

}  // namespace

namespace detail {

class EPollObserver : public PollObserver {
 public:
  friend EPollFile;

  EPollObserver(EPollFile &epollf, File &f, int32_t watched_events,
                uint64_t user_data, bool one_shot_triggered = false)
      : one_shot_triggered_(one_shot_triggered),
        epollf_(&epollf),
        f_(&f),
        watched_events_(watched_events),
        user_data_(user_data) {}
  ~EPollObserver() override;

  EPollObserver(const EPollObserver &o) noexcept = delete;
  EPollObserver &operator=(const EPollObserver &o) = delete;
  EPollObserver(EPollObserver &&o) noexcept = delete;
  EPollObserver &operator=(EPollObserver &&o) = delete;

 private:
  void Notify(unsigned int event_mask) override;

  friend void junction::EpollObserverSave(cereal::BinaryOutputArchive &ar,
                                          const PollObserver &o);
  friend void junction::EpollObserverLoad(cereal::BinaryInputArchive &ar);

  bool attached_{false};  // TODO(amb): switch to better intrusive list?
  bool one_shot_triggered_;
  EPollFile *epollf_;
  File *f_;
  uint32_t watched_events_;
  uint32_t triggered_events_{0};
  uint64_t user_data_;
  IntrusiveListNode node_;
};

class EPollFile : public File {
 public:
  EPollFile() : File(FileType::kSpecial, 0, FileMode::kRead) {}
  ~EPollFile();

  static void Notify(PollSource &s);

  bool Add(File &f, uint32_t events, uint64_t user_data,
           bool one_shot_triggered = false, uint32_t triggered_events = 0);
  bool Modify(File &f, uint32_t events, uint64_t user_data);
  bool Delete(File &f);
  int Wait(std::span<epoll_event> events, std::optional<Duration> timeout,
           std::optional<k_sigset_t> mask);

  void AddEvent(EPollObserver &o) {
    rt::SpinGuard g(lock_);
    if (std::exchange(o.attached_, true)) return;
    events_.push_back(o);
    waker_.Wake();
  }

  void RemoveEvent(EPollObserver &o) {
    rt::SpinGuard g(lock_);
    if (!std::exchange(o.attached_, false)) return;
    events_.erase(decltype(events_)::s_iterator_to(o));
  }

 private:
  friend class cereal::access;

  template <class Archive>
  void save(Archive &ar) const {
    ar(cereal::base_class<File>(this));
  }

  template <class Archive>
  void load(Archive &ar) {
    ar(cereal::base_class<File>(this));
  }

  int DeliverEvents(std::span<epoll_event> events_out);

  rt::Spin lock_;
  rt::ThreadWaker waker_;
  IntrusiveList<EPollObserver, &EPollObserver::node_> events_;
};

EPollFile::~EPollFile() {
  Process::ForEachProcess([this](Process &p) {
    FileTable &ftbl = p.get_file_table();
    ftbl.ForEach([this](File &f, int fd) { Delete(f); });
  });
}

void EPollFile::Notify(PollSource &s) {
  bool exclusive = false;
  for (auto &o : s.epoll_observers_) {
    auto &oe = static_cast<EPollObserver &>(o);
    if ((oe.watched_events_ & kEPollOneShot) != 0) {
      if (std::exchange(oe.one_shot_triggered_, true)) continue;
    }
    if ((oe.watched_events_ & kEPollExclusive) != 0) {
      if (std::exchange(exclusive, true)) continue;
    }
    oe.Notify(s.get_events());
  }
}

bool EPollFile::Add(File &f, uint32_t events, uint64_t user_data,
                    bool one_shot_triggered, uint32_t triggered_events) {
  events |= (kPollHUp | kPollErr);  // can't be ignored
  auto o = std::make_unique<EPollObserver>(*this, f, events, user_data,
                                           one_shot_triggered);
  PollSource &src = f.get_poll_source();
  rt::SpinGuard guard(src.lock_);
  for (const auto &o : src.epoll_observers_)
    if (unlikely(static_cast<const EPollObserver &>(o).epollf_ == this))
      return false;
  src.epoll_observers_.push_back(*o);
  o->Notify(src.get_events() | triggered_events);
  o.release();
  return true;
}

bool EPollFile::Modify(File &f, uint32_t events, uint64_t user_data) {
  events |= (kPollHUp | kPollErr);  // can't be ignored
  PollSource &src = f.get_poll_source();
  rt::SpinGuard guard(src.lock_);
  for (auto &o : src.epoll_observers_) {
    auto &oe = static_cast<EPollObserver &>(o);
    if (oe.epollf_ != this) continue;
    oe.watched_events_ = events;
    oe.user_data_ = user_data;
    oe.one_shot_triggered_ = false;
    oe.Notify(oe.triggered_events_);
    return true;
  }
  return false;
}

bool EPollFile::Delete(File &f) {
  PollSource &src = f.get_poll_source();
  rt::SpinGuard guard(src.lock_);
  auto it = src.epoll_observers_.begin();
  while (it != src.epoll_observers_.end()) {
    auto &oe = static_cast<EPollObserver &>(*it);
    if (oe.epollf_ == this) {
      src.epoll_observers_.erase_and_dispose(it,
                                             [](PollObserver *o) { delete o; });
      return true;
    }
    ++it;
  }
  return false;
}

int EPollFile::DeliverEvents(std::span<epoll_event> events_out) {
  assert(lock_.IsHeld());
  auto it = events_out.begin();
  IntrusiveList<EPollObserver, &EPollObserver::node_> tmp;
  while (!events_.empty() && it != events_out.end()) {
    EPollObserver &o = events_.front();
    it->events = o.triggered_events_ & o.watched_events_;
    it->data.u64 = o.user_data_;
    ++it;
    events_.pop_front();
    if ((o.watched_events_ & kEPollEdgeTriggered) != 0) {
      o.attached_ = false;
      continue;
    }
    tmp.push_back(o);
  }
  // Put the events that were delivered at the end for better fairness.
  events_.splice(events_.end(), tmp);
  return std::distance(events_out.begin(), it);
}

int EPollFile::Wait(std::span<epoll_event> events_out,
                    std::optional<Duration> timeout,
                    std::optional<k_sigset_t> mask) {
  // Fast path: Check if events are ready now
  {
    rt::SpinGuard g(lock_);
    if (!events_.empty()) return DeliverEvents(events_out);
  }
  if (timeout && timeout->IsZero()) return 0;

  // Slow path: Block and wait for events
  rt::WakeOnTimeout timed_out(lock_, waker_, timeout);
  SigMaskGuard sig(mask);
  bool signaled;
  {
    rt::SpinGuard g(lock_);
    signaled = !rt::WaitInterruptible(lock_, waker_, [this, &timed_out] {
      return !events_.empty() || timed_out;
    });
    if (!events_.empty()) return DeliverEvents(events_out);
  }

  if (signaled) return timeout ? -EINTR : -ERESTARTNOHAND;
  return 0;
}

void EPollObserver::Notify(uint32_t events) {
  triggered_events_ = events;
  if ((triggered_events_ & watched_events_) != 0)
    epollf_->AddEvent(*this);
  else
    epollf_->RemoveEvent(*this);
}

EPollObserver::~EPollObserver() { epollf_->RemoveEvent(*this); }

}  // namespace detail

namespace {

// Creates a new EPoll file.
int CreateEPollFile(bool cloexec = false) {
  auto f = std::make_shared<detail::EPollFile>();
  return myproc().get_file_table().Insert(std::move(f), cloexec);
}

int DoEPollWait(int epfd, epoll_event *events, int maxevents,
                std::optional<Duration> timeout,
                std::optional<k_sigset_t> mask = std::nullopt) {
  if (unlikely(maxevents < 0)) return -EINVAL;
  FileTable &ftbl = myproc().get_file_table();
  File *f = ftbl.Get(epfd);
  if (unlikely(!f)) return -EBADF;
  auto *epf = most_derived_cast<detail::EPollFile>(f);
  if (unlikely(!epf)) return -EINVAL;
  return epf->Wait({events, static_cast<size_t>(maxevents)}, timeout, mask);
}

}  // namespace

void PollSource::Notify() {
  rt::SpinGuard guard(lock_);
  for (auto &o : observers_) o.Notify(event_mask_);
  detail::EPollFile::Notify(*this);
}

void PollSource::DetachEPollObservers() {
  epoll_observers_.erase_and_dispose(epoll_observers_.begin(),
                                     epoll_observers_.end(),
                                     [](PollObserver *o) { delete o; });
}

void EpollObserverSave(cereal::BinaryOutputArchive &ar, const PollObserver &o) {
  assert(dynamic_cast_guarded<const detail::EPollObserver *>(&o) != nullptr);
  auto &oe = static_cast<const detail::EPollObserver &>(o);
  auto epf = std::static_pointer_cast<detail::EPollFile>(
      oe.epollf_->shared_from_this());
  std::shared_ptr<File> file = oe.f_->shared_from_this();
  ar(epf, file, oe.watched_events_, oe.user_data_, oe.one_shot_triggered_,
     oe.triggered_events_);
}

void EpollObserverLoad(cereal::BinaryInputArchive &ar) {
  std::shared_ptr<detail::EPollFile> epf;
  std::shared_ptr<File> file;
  uint32_t watched_events;
  uint32_t triggered_events;
  uint64_t user_data;
  bool one_shot_triggered;
  ar(epf, file, watched_events, user_data, one_shot_triggered,
     triggered_events);
  epf->Add(*file.get(), watched_events, user_data, one_shot_triggered,
           triggered_events);
}

long usys_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  if (timeout < 0) return DoPoll(fds, nfds, {});
  return DoPoll(fds, nfds,
                Duration(static_cast<uint64_t>(timeout) * kMilliseconds));
}

long usys_ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *ts,
                const sigset_t *sigmask, size_t sigsetsize) {
  if (!ts) return DoPoll(fds, nfds, {});
  return DoPoll(fds, nfds, Duration(*ts), KernelSigset(sigmask));
}

long usys_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                 struct timeval *tv) {
  std::optional<Duration> d;
  if (tv) d = Duration(*tv);
  auto [ret, left] = DoSelect(nfds, readfds, writefds, exceptfds, d);
  if (ret >= 0 && tv) *tv = left.Timeval();
  return ret;
}

// pselect6's sixth argument is not the signal set. The Linux ABI packs it:
// a pointer to {const sigset_t *ss; size_t ss_len;}, because the syscall has
// run out of argument registers. glibc builds exactly that. Reading a sigset
// here instead used the *address* of the mask as the mask -- so which signals
// were blocked during the wait depended on the stack layout, and GNU make's
// jobserver wait (pselect with SIGCHLD unblocked, on every -jN build) landed
// on an address with bit 16 set, blocked SIGCHLD, and slept forever.
struct sigset_argpack {
  const sigset_t *ss;
  size_t ss_len;
};

long usys_pselect6(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, struct timespec *ts,
                   const sigset_t *sigmask_argpack) {
  // Declared as sigset_t* to match the generated syscall table; see above for
  // what actually arrives.
  const auto *sig = reinterpret_cast<const sigset_argpack *>(sigmask_argpack);
  std::optional<Duration> d;
  if (ts) d = Duration(*ts);
  const sigset_t *sigmask = nullptr;
  if (sig && sig->ss) {
    if (unlikely(sig->ss_len != sizeof(k_sigset_t))) return -EINVAL;
    sigmask = sig->ss;
  }
  auto [ret, left] =
      DoSelect(nfds, readfds, writefds, exceptfds, d, KernelSigset(sigmask));
  if (ret >= 0 && ts) *ts = left.Timespec();
  return ret;
}

// This variant is deprecated, and size is ignored.
long usys_epoll_create(int size) { return CreateEPollFile(); }

long usys_epoll_create1(int flags) {
  return CreateEPollFile((flags & kFlagCloseExec) > 0);
}

long usys_epoll_ctl(int epfd, int op, int fd, const struct epoll_event *event) {
  // get the epoll file
  FileTable &ftbl = myproc().get_file_table();
  File *f = ftbl.Get(epfd);
  if (unlikely(!f)) return -EBADF;
  auto *epf = most_derived_cast<detail::EPollFile>(f);
  if (unlikely(!epf)) return -EINVAL;

  // get the monitored file
  f = ftbl.Get(fd);
  if (unlikely(!f)) return -EBADF;

  // handle the operation
  switch (op) {
    case EPOLL_CTL_ADD:
      if (!epf->Add(*f, event->events, event->data.u64)) return -EEXIST;
      break;
    case EPOLL_CTL_MOD:
      if (!epf->Modify(*f, event->events, event->data.u64)) return -ENOENT;
      break;
    case EPOLL_CTL_DEL:
      if (!epf->Delete(*f)) return -ENOENT;
      break;
    default:
      return -EINVAL;
  }

  return 0;
}

long usys_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                     int timeout_ms) {
  std::optional<Duration> timeout;
  if (timeout_ms >= 0)
    timeout = Duration(static_cast<uint64_t>(timeout_ms) * kMilliseconds);
  return DoEPollWait(epfd, events, maxevents, timeout);
}

long usys_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                      int timeout_ms, const sigset_t *sigmask,
                      size_t sigsetsize) {
  std::optional<Duration> timeout;
  if (timeout_ms >= 0)
    timeout = Duration(static_cast<uint64_t>(timeout_ms) * kMilliseconds);
  return DoEPollWait(epfd, events, maxevents, timeout, KernelSigset(sigmask));
}

long usys_epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
                       const struct timespec *ts, const sigset_t *sigmask,
                       size_t sigsetsize) {
  std::optional<Duration> timeout;
  if (ts) timeout = Duration(*ts);
  return DoEPollWait(epfd, events, maxevents, timeout, KernelSigset(sigmask));
}

}  // namespace junction

CEREAL_REGISTER_TYPE(junction::detail::EPollFile);
