#include "junction/kernel/itimer.h"

#include "junction/bindings/log.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/usys.h"

namespace junction {

// This function is called from the softirq thread with preemption disabled
void ITimer::Run() {
  if (next_fire_) proc_.Signal(SIGALRM);
  if (interval_.IsZero()) {
    next_fire_ = std::nullopt;
    return;
  }
  next_fire_ = Time::Now() + interval_;
  timer_restart(&entry_, next_fire_->Microseconds());
}

// Called from the softirq thread with preemption disabled, like ITimer::Run.
void PosixTimer::Run() {
  if (next_fire_ && sev_.sigev_notify != SIGEV_NONE) {
    siginfo_t si;
    std::memset(&si, 0, sizeof(si));
    si.si_signo = sev_.sigev_signo;
    si.si_code = SI_TIMER;
    si.si_timerid = id_;
    si.si_overrun = 0;
    si.si_value = sev_.sigev_value;
    if (sev_.sigev_notify & SIGEV_THREAD_ID)
      (void)proc_.SignalThread(sev_._sigev_un._tid, si);
    else
      proc_.Signal(si);
  }
  if (interval_.IsZero()) {
    next_fire_ = std::nullopt;
    return;
  }
  next_fire_ = Time::Now() + interval_;
  timer_restart(&entry_, next_fire_->Microseconds());
}

long usys_timer_create([[maybe_unused]] clockid_t clockid, struct sigevent *sevp,
                       int *timerid) {
  if (!timerid) return -EFAULT;
  struct sigevent sev;
  std::memset(&sev, 0, sizeof(sev));
  if (sevp) {
    sev = *sevp;
    int how = sev.sigev_notify & ~SIGEV_THREAD_ID;
    if (how != SIGEV_SIGNAL && how != SIGEV_NONE) return -EINVAL;
    if (how == SIGEV_SIGNAL && !SignalValid(sev.sigev_signo)) return -EINVAL;
  } else {
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
  }
  Process &p = myproc();
  Status<int> id = p.get_posix_timers().Create(p, sev);
  if (!id) return MakeCError(id);
  // The default sigev_value is the timer's id, known only now; nothing reads
  // it for a default timer but the handler, so it is set by re-creating.
  if (!sevp) {
    p.get_posix_timers().Delete(*id);
    sev.sigev_value.sival_int = *id;
    id = p.get_posix_timers().Create(p, sev);
    if (!id) return MakeCError(id);
  }
  *timerid = *id;
  return 0;
}

long usys_timer_settime(int timerid, int flags, const struct itimerspec *nv,
                        struct itimerspec *ov) {
  if (!nv) return -EFAULT;
  Duration d(nv->it_value);
  if ((flags & TIMER_ABSTIME) && !d.IsZero()) {
    // Absolute on the realtime clock is the only absolute time that means
    // anything here; the others run at the same rate, so a deadline already
    // expressed against them is converted through the present.
    Duration until = Duration::Until(Time::FromUnixTime(nv->it_value));
    d = until.Microseconds() > 0 ? until : Duration(1);
  }
  struct itimerspec old;
  bool found = myproc().get_posix_timers().With(timerid, [&](PosixTimer &t) {
    old = t.exchange(d, Duration(nv->it_interval));
  });
  if (!found) return -EINVAL;
  if (ov) *ov = old;
  return 0;
}

long usys_timer_gettime(int timerid, struct itimerspec *cur) {
  if (!cur) return -EFAULT;
  bool found = myproc().get_posix_timers().With(
      timerid, [&](PosixTimer &t) { *cur = t.get(); });
  return found ? 0 : -EINVAL;
}

long usys_timer_getoverrun(int timerid) {
  bool found = myproc().get_posix_timers().With(timerid, [](PosixTimer &) {});
  return found ? 0 : -EINVAL;
}

long usys_timer_delete(int timerid) {
  return myproc().get_posix_timers().Delete(timerid) ? 0 : -EINVAL;
}

long usys_setitimer(int which, const struct itimerval *new_value,
                    struct itimerval *old_value) {
  if (unlikely(which != ITIMER_REAL)) {
    LOG_ONCE(ERR) << "Only ITIMER_REAL is supported";
    return -EINVAL;
  }

  // Linux incorrectly assumes that a null new_value means cancel the timer.
  // We do the same...
  if (!new_value) {
    static const itimerval null_val = {{0, 0}, {0, 0}};
    new_value = &null_val;
  }

  itimerval old = myproc().get_itimer().exchange(*new_value);
  if (old_value) *old_value = old;

  return 0;
}

long usys_getitimer(int which, struct itimerval *curr_value) {
  if (unlikely(which != ITIMER_REAL)) return -EINVAL;
  *curr_value = myproc().get_itimer().get();
  return 0;
}

long usys_alarm(unsigned int seconds) {
  itimerval val = {{0, 0}, {seconds, 0}};
  myproc().get_itimer().exchange(val);
  return 0;
}

}  // namespace junction
