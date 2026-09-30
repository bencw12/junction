#pragma once

#include <signal.h>
#include <time.h>

#include <map>
#include <memory>
#include <optional>

#include "junction/bindings/log.h"
#include "junction/bindings/timer.h"

namespace junction {

class Process;

class ITimer : private rt::timer_internal::timer_node {
 public:
  explicit ITimer(Process &proc) : proc_(proc) {
    auto arg = reinterpret_cast<unsigned long>(static_cast<timer_node *>(this));
    timer_init(&entry_, rt::timer_internal::TimerTrampoline, arg);
  }

  ~ITimer() { timer_cancel_recurring(&entry_); }

  itimerval get() const {
    Duration d = next_fire_ ? Duration::Until(*next_fire_) : Duration(0);
    return {interval_.Timeval(), d.Timeval()};
  }

  itimerval exchange(const itimerval &it) {
    // synchronize with the timer callback
    timer_cancel_recurring(&entry_);

    itimerval old = get();

    // record new interval
    interval_ = Duration(it.it_interval);

    // check if we need to start the timer
    Duration d(it.it_value);
    if (d.IsZero()) {
      next_fire_ = std::nullopt;
    } else {
      next_fire_ = Time::Now() + d;
      timer_start(&entry_, next_fire_->Microseconds());
    }

    return old;
  }

  // disable copy and move.
  ITimer(const ITimer &) = delete;
  ITimer &operator=(const ITimer &) = delete;
  ITimer(const ITimer &&) = delete;
  ITimer &operator=(const ITimer &&) = delete;

 private:
  void Run() override;
  timer_entry entry_;
  Duration interval_{0};
  std::optional<Time> next_fire_{std::nullopt};
  Process &proc_;
};

// A POSIX per-process timer (timer_create). It fires by signal: to the
// process, or with SIGEV_THREAD_ID to one thread -- which is also how glibc
// builds SIGEV_THREAD, on a helper thread that waits for SIGTIMER and reads the
// timer out of si_value. vim arms one of those for its escape-sequence timeout,
// and without timer_create it reports E1286 and sleeps a second after every
// lone ESC.
class PosixTimer : private rt::timer_internal::timer_node {
 public:
  PosixTimer(Process &proc, int id, const struct sigevent &sev)
      : proc_(proc), id_(id), sev_(sev) {
    auto arg = reinterpret_cast<unsigned long>(static_cast<timer_node *>(this));
    timer_init(&entry_, rt::timer_internal::TimerTrampoline, arg);
  }
  ~PosixTimer() { timer_cancel_recurring(&entry_); }

  itimerspec get() const {
    Duration d = next_fire_ ? Duration::Until(*next_fire_) : Duration(0);
    if (next_fire_ && d.Microseconds() <= 0) d = Duration(1);
    return {interval_.Timespec(), d.Timespec()};
  }

  // Arms (or, with a zero value, disarms) the timer to fire @d from now.
  itimerspec exchange(Duration d, Duration interval) {
    timer_cancel_recurring(&entry_);  // synchronizes with Run()
    itimerspec old = get();
    interval_ = interval;
    if (d.IsZero()) {
      next_fire_ = std::nullopt;
    } else {
      next_fire_ = Time::Now() + d;
      timer_start(&entry_, next_fire_->Microseconds());
    }
    return old;
  }

  PosixTimer(const PosixTimer &) = delete;
  PosixTimer &operator=(const PosixTimer &) = delete;

 private:
  void Run() override;
  timer_entry entry_;
  Duration interval_{0};
  std::optional<Time> next_fire_{std::nullopt};
  Process &proc_;
  const int id_;
  const struct sigevent sev_;
};

// A process's POSIX timers. They are not inherited by fork() and do not
// survive execve().
class PosixTimerTable {
 public:
  Status<int> Create(Process &proc, const struct sigevent &sev) {
    rt::SpinGuard g(lock_);
    int id = 0;
    while (timers_.count(id)) id++;
    if (id >= kMaxTimers) return MakeError(EAGAIN);
    timers_[id] = std::make_unique<PosixTimer>(proc, id, sev);
    return id;
  }
  bool Delete(int id) {
    std::unique_ptr<PosixTimer> t;
    {
      rt::SpinGuard g(lock_);
      auto it = timers_.find(id);
      if (it == timers_.end()) return false;
      t = std::move(it->second);
      timers_.erase(it);
    }
    return true;  // destroyed here, outside the lock: it waits for Run()
  }
  // Runs @fn on timer @id, if there is one.
  template <typename F>
  bool With(int id, F fn) {
    rt::SpinGuard g(lock_);
    auto it = timers_.find(id);
    if (it == timers_.end()) return false;
    fn(*it->second);
    return true;
  }
  void Clear() {
    std::map<int, std::unique_ptr<PosixTimer>> dead;
    {
      rt::SpinGuard g(lock_);
      dead.swap(timers_);
    }
  }

 private:
  static constexpr int kMaxTimers = 64;
  rt::Spin lock_;
  std::map<int, std::unique_ptr<PosixTimer>> timers_;
};

}  // namespace junction
