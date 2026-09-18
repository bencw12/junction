extern "C" {
#include <runtime/interruptible_wait.h>
#include <asm/prctl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "lib/caladan/runtime/defs.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <linux/sched.h>
}

#include <cstdlib>
#include <cstring>

#include "junction/base/arch.h"
#include "junction/base/bits.h"
#include "junction/bindings/log.h"
#include "junction/bindings/runtime.h"
#include "junction/bindings/sync.h"
#include "junction/bindings/timer.h"
#include "junction/junction.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/as.h"
#include "junction/kernel/memtrace.h"
#include "junction/kernel/futex.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/memprobe.h"
#include "junction/kernel/mm.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/usys.h"
#include "junction/limits.h"
#include "junction/snapshot/snapshot.h"
#include "junction/syscall/entry.h"
#include "junction/syscall/strace.h"
#include "junction/syscall/syscall.h"

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

namespace junction {

inline constexpr uint64_t kThreadRequiredFlags =
    (CLONE_VM | CLONE_FILES | CLONE_FS | CLONE_THREAD | CLONE_SIGHAND);

inline constexpr uint64_t kVforkRequiredFlags = (CLONE_VM | CLONE_VFORK);

inline constexpr uint64_t kCheckFlags =
    (kThreadRequiredFlags | kVforkRequiredFlags);

// Global allocation of PIDs
rt::Spin process_lock;
UIDGenerator<kMaxProcesses> pid_generator;
std::map<pid_t, unsigned long> pid_ref_count;
std::shared_ptr<Process> init_proc;

void DecRefCountPid(pid_t pid) {
  assert(process_lock.IsHeld());
  if (--pid_ref_count[pid] == 0) {
    pid_generator.Release(pid);
    pid_ref_count.erase(pid);
  }
}

void IncRefCountPid(pid_t pid, int incr = 1) {
  assert(process_lock.IsHeld());
  pid_ref_count[pid] += incr;
}

Status<pid_t> AllocPid(std::optional<pid_t> pgid = std::nullopt,
                       std::optional<pid_t> sid = std::nullopt) {
  rt::SpinGuard guard(process_lock);
  std::optional<size_t> tmp = pid_generator();
  if (!tmp) return MakeError(ENOSPC);
  pid_ref_count[*tmp] = 1;
  if (pgid) IncRefCountPid(*pgid);
  if (sid) IncRefCountPid(*sid);
  return *tmp;
}

Status<pid_t> AllocNewSession() {
  rt::SpinGuard guard(process_lock);
  std::optional<size_t> tmp = pid_generator();
  if (!tmp) return MakeError(ENOSPC);
  pid_ref_count[*tmp] = 3;
  return *tmp;
}

void ReleasePid(pid_t pid, std::optional<pid_t> pgid = std::nullopt,
                std::optional<pid_t> sid = std::nullopt) {
  rt::SpinGuard guard(process_lock);
  DecRefCountPid(pid);
  if (pgid) DecRefCountPid(*pgid);
  if (sid) DecRefCountPid(*sid);
}

void AcquirePid(pid_t pid, std::optional<pid_t> pgid,
                std::optional<pid_t> sid) {
  rt::SpinGuard guard(process_lock);
  pid_generator.Acquire(pid);
  IncRefCountPid(pid);
  if (pgid) IncRefCountPid(*pgid);
  if (sid) IncRefCountPid(*sid);
}

Credential::Credential()
    : ruid(GetCfg().get_uid()),
      euid(GetCfg().get_uid()),
      suid(GetCfg().get_uid()),
      rgid(GetCfg().get_gid()),
      egid(GetCfg().get_gid()),
      sgid(GetCfg().get_gid()) {}

void SetInitProc(std::shared_ptr<Process> proc) {
  assert(!init_proc);
  init_proc = std::move(proc);
}

long DoClone(clone_args *cl_args, uint64_t rsp) {
  bool do_vfork = false;
  bool do_fork = false;

  switch (cl_args->flags & kCheckFlags) {
    case kVforkRequiredFlags:
      do_vfork = true;
      break;
    case kThreadRequiredFlags:
      break;
    case 0:
      // Nothing is shared with the caller, so this is a fork: the child needs
      // an address space of its own holding a copy of the caller's memory.
      do_fork = true;
      break;
    default:
      return -ENOSYS;
  }

  Status<Thread *> tptr;

  Thread &oldth = mythread();

  if (do_vfork) {
    rt::ThreadWaker waker;
    waker.Arm();
    Status<std::shared_ptr<Process>> forkp =
        oldth.get_process().CreateProcessVfork(std::move(waker));
    if (!forkp) return MakeCError(forkp);
    tptr = (*forkp)->CreateThreadMain(oldth);
  } else if (do_fork) {
    Status<std::shared_ptr<Process>> forkp =
        oldth.get_process().CreateProcessFork(*cl_args);
    if (!forkp) return MakeCError(forkp);
    tptr = (*forkp)->CreateThreadMain(oldth);
  } else {
    tptr = oldth.get_process().CreateThread(oldth);
  }

  if (!tptr) return MakeCError(tptr);

  Thread &newth = **tptr;
  thread_t &new_cth = *newth.GetCaladanThread();
  thread_t &old_cth = *oldth.GetCaladanThread();

  // Clone the trap frame
  uint64_t sys_rsp = newth.get_syscall_stack_rsp();
  SyscallFrame &restore_tf = oldth.GetSyscallFrame().CloneTo(&sys_rsp);
  restore_tf.SetRax(0, rsp);
  newth.mark_enter_kernel();
  restore_tf.MakeUnwinderSysret(newth, new_cth.tf);

  // Set FSBASE if requested.
  if (cl_args->flags & CLONE_SETTLS) {
    thread_set_fsbase(&new_cth, cl_args->tls);
  } else {
    new_cth.has_fsbase = old_cth.has_fsbase;
    new_cth.fsbase = old_cth.fsbase;
  }

  // Write this thread's tid into the requested location.
  if (cl_args->flags & CLONE_PARENT_SETTID)
    *reinterpret_cast<uint32_t *>(cl_args->parent_tid) = newth.get_tid();

  // Save a pointer to the child_tid address if requested, so it can later
  // notify the parent of the child's exit via futex.
  if (cl_args->flags & CLONE_CHILD_CLEARTID)
    newth.set_child_tid(reinterpret_cast<uint32_t *>(cl_args->child_tid));

  if (do_vfork) {
    bool was_stopped = myproc().is_stopped();
    if (likely(!was_stopped)) {
      myproc().JobControlStop();
      myproc().WaitForFullStop();
    }
    if (unlikely(GetCfg().strace_enabled()))
      LogSyscallDirect(newth.get_tid(), "vfork");
    {
      rt::Preempt::Lock();
      newth.ThreadReady();
      rt::Preempt::UnlockAndPark();
    }
    // Wait for child thread to exit or exec.
    if (!was_stopped) myproc().JobControlContinue();
  } else {
    newth.ThreadReady();
  }

  return newth.get_tid();
}

void Thread::DestroyThread(Thread *th) {
  assert(th->cold().ref_count_ == 0);
  thread_t *cth = th->GetCaladanThread();
  assert(cth != thread_self());
  th->~Thread();
  // Ensure the thread is no longer scheduled.
  if (unlikely(load_acquire(&cth->thread_running))) {
    /* wait until the scheduler finishes switching stacks */
    while (load_acquire(&cth->thread_running)) cpu_relax();
  }
  thread_free(cth);
}

Thread::~Thread() {
  uint32_t *child_tid = get_child_tid();
  if (child_tid) {
    // This writes *guest* memory, and DestroyThread() guarantees we are not
    // the dying thread -- so the core may well be bound to some other
    // process's address space. If it is, the zero lands at this address in the
    // wrong address space: the waiter in the right one never sees its tid
    // cleared, and the FUTEX_WAKE below wakes it only to find the value
    // unchanged and block again. That is a hang, and it is indistinguishable
    // from a lost wakeup unless you look here.
    uint64_t want = proc_->get_mem_map().get_as_handle();
    if (unlikely(GetActiveAddressSpace() != want)) {
      static std::atomic_int reported{0};
      if (reported.fetch_add(1, std::memory_order_relaxed) < 16)
        LOG(WARN) << "thread exit: clear_child_tid write for tid " << tid_
                  << " (pid " << proc_->get_pid() << ") in address space "
                  << GetActiveAddressSpace() << ", want " << want;
    }
    *child_tid = 0;
    FutexTable::GetFutexTable().Wake(child_tid);
  }
  bool proc_done = proc_->ThreadFinish(this);
  if (tid_ != proc_->get_pid()) ReleasePid(tid_);
  if (proc_done) proc_->ProcessFinish();
  DestroyCold();
}

bool Thread::IsStopped() const { return proc_->is_stopped(); }

Status<void> Thread::DropUnusedStack() {
  return {};
  assert(get_process().is_fully_stopped());
  // If the thread has indicated that signals cannot be delivered on it then we
  // shouldn't assume that we can clobber above rsp.
  if (get_sighand().has_altstack()) return {};

  // Check if the rsp is on a known stack.
  uint64_t rsp = GetTrapframe().GetRsp();
  MemoryMap &mm = get_process().get_mem_map();
  std::optional<void *> top = mm.GetStackTop(rsp);
  if (!top) return {};

  // Check if the syscall is performed on top of the rsp.
  uint64_t resume_rsp = GetCaladanThread()->tf.rsp;
  std::optional<void *> resume_top = mm.GetStackTop(resume_rsp);
  if (resume_top && *resume_top == *top) rsp = resume_rsp;

  // Drop pages above the redzone.
  rsp = PageAlignDown(rsp - kRedzoneSize);
  uint64_t len = rsp - reinterpret_cast<uintptr_t>(*top);
  return KernelMAdvise(*top, len, MADV_DONTNEED);
}

Status<pid_t> Process::BecomeSessionLeader() {
  rt::SpinGuard g(process_lock);
  if (is_process_group_leader()) return MakeError(EPERM);
  IncRefCountPid(pid_, 2);
  DecRefCountPid(sid_);
  DecRefCountPid(pgid_);
  sid_ = pid_;
  pgid_ = pid_;
  return pid_;
}

Status<void> Process::JoinProcessGroup(pid_t pgid) {
  assert(pgid > 0);
  rt::SpinGuard g(process_lock);
  if (is_process_group_leader()) return MakeError(EPERM);
  DecRefCountPid(pgid_);
  IncRefCountPid(pgid);
  pgid_ = pgid;
  return {};
}

void Process::ProcessFinish() {
  if (unlikely(!mem_map_->DumpTracerReport())) {
    LOG(ERR) << "Failed to dump memory trace";
    syscall_exit(-1);
  }
  // Check if init has died
  if (unlikely(init_proc.get() == this)) {
    syscall_exit(xstate_);
    std::unreachable();
  }

  // Close all file descriptors since ftbl destructor is not called until
  // process is reaped, but a parent might be blocked on the other end of a
  // pipe.
  get_file_table().Destroy();

  // safe to access child_procs_ with no lock since the process is dead

  if (!child_procs_.size()) return;

  // reparent
  rt::SpinGuard g(init_proc->shared_sig_q_);
  for (auto &child : child_procs_) {
    rt::SpinGuard g(child->shared_sig_q_);
    child->parent_ = init_proc;
    init_proc->child_procs_.push_back(std::move(child));
  }
}

rt::Spin Process::pid_map_lock_;
std::map<pid_t, Process *> Process::pid_to_proc_;
Process::~Process() {
  DeregisterProcess(*this);
  AdvisoryLockMap::Get().NotifyProcDestroy(get_pid());
  ReleasePid(pid_, pgid_, sid_);
}

Status<void> Process::WaitForFullStop() {
  bool this_thread_stopped = IsJunctionThread() && this == &myproc();
  rt::SpinGuard g(child_thread_lock_);
  if (this_thread_stopped) stopped_count_ += 1;
  rt::Wait(child_thread_lock_, stopped_threads_,
           [&]() { return stopped_count_ == thread_map_.size() || exited_; });
  if (this_thread_stopped) stopped_count_ -= 1;
  if (exited_) return MakeError(ESRCH);
  return {};
}

Status<void> Process::WaitForNthStop(size_t stopcount) {
  rt::SpinGuard g(child_thread_lock_);
  rt::Wait(child_thread_lock_, stopped_threads_,
           [&]() { return stop_cnt_ >= stopcount || exited_; });
  if (exited_) return MakeError(ESRCH);
  return {};
}

void Process::FinishExec(std::shared_ptr<MemoryMap> &&new_mm) {
  {
    rt::SpinGuard g(child_thread_lock_);

    // Kill any threads besides this one.
    for (const auto &[pid, th] : thread_map_)
      if (pid != mythread().get_tid()) th->Kill();

    // Wait for other threads to exit.
    rt::Wait(child_thread_lock_, exec_waker_,
             [this] { return thread_map_.size() == 1; });
  }

  file_tbl_.DoCloseOnExec();
  mem_map_ = std::move(new_mm);
  vfork_waker_.Wake();
}

bool Process::ThreadFinish(Thread *th) {
  rt::SpinGuard g(child_thread_lock_);
  accumulated_runtime_ += th->GetRuntime();
  thread_map_.erase(th->get_tid());
  size_t remaining_threads = thread_map_.size();
  if (remaining_threads == 1) exec_waker_.Wake();
  if (is_stopped() && stopped_count_ == remaining_threads)
    NotifyParentWait(kWaitableStopped);
  return remaining_threads == 0;
}

Status<std::pair<std::shared_ptr<Process>, Thread *>> Process::CreateInit(
    FSRoot &fsr) {
  Status<pid_t> pid = AllocNewSession();
  if (!pid) return MakeError(pid);

  Status<std::shared_ptr<MemoryMap>> mm = MemoryMap::Create(kMemoryMappingSize);
  if (!mm) return MakeError(mm);

  auto proc = std::make_shared<Process>(*pid, std::move(*mm), fsr);

  thread_t *th = thread_create(nullptr, 0);
  if (!th) return MakeError(ENOMEM);

  Thread *tstate = reinterpret_cast<Thread *>(th->junction_tstate_buf);
  new (tstate) Thread(proc, *pid);
  th->junction_thread = true;
  th->has_fsbase = true;
  th->fsbase = 0;
  proc->thread_map_[*pid] = tstate;
  return {{std::move(proc), tstate}};
}

Status<std::shared_ptr<Process>> Process::CreateProcessVfork(
    rt::ThreadWaker &&w) {
  Status<pid_t> pid = AllocPid(get_pgid(), get_sid());
  if (!pid) return MakeError(pid);

  auto p = std::make_shared<Process>(*pid, mem_map_, file_tbl_, std::move(w),
                                     shared_from_this(), get_pgid(), get_fs(),
                                     get_sid());
  rt::SpinGuard g(shared_sig_q_);
  child_procs_.push_back(p);
  return p;
}

Status<std::shared_ptr<Process>> Process::CreateProcessFork(
    const clone_args &cl_args) {
  if (unlikely(!MultiAddressSpaceEnabled())) return MakeError(ENOSYS);

  // Serialize forks. Which ranges are left out of a clone is a property of the
  // address space (MADV_DONTFORK is a VMA flag), so two clones cannot be in
  // flight at once without stepping on each other's exclusions.
  rt::MutexGuard fg(AddressSpaceForkLock());

  Status<pid_t> pid = AllocPid(get_pgid(), get_sid());
  if (!pid) return MakeError(pid);

  // Every other guest's memory must stay out of the child's address space.
  // Only other processes' memory maps qualify: the global region table also
  // holds ranges the LibOS registered for itself, and those have to be present
  // in every address space.
  AddressRange own = mem_map_->get_reservation();
  std::vector<AddressRange> exclude;
  ForEachProcess([&](Process &p) {
    AddressRange r = p.get_mem_map().get_reservation();
    if (r.start == own.start) return;  // ours, or a vfork child sharing it
    for (const AddressRange &e : exclude)
      if (e.start == r.start) return;
    exclude.push_back(r);
  });

  // CLONE_CHILD_SETTID asks for the child's tid to be stored in the child's
  // memory. The child's memory does not exist yet, and once it does it is not
  // reachable from here -- so write the value now and let the clone inherit
  // it, then put the caller's own value back. Only the calling thread can see
  // this location (it is in its TLS), so the window is harmless.
  uint32_t *ctid = nullptr;
  uint32_t saved_ctid = 0;
  if (cl_args.flags & CLONE_CHILD_SETTID) {
    ctid = reinterpret_cast<uint32_t *>(cl_args.child_tid);
    if (ctid) {
      saved_ctid = *ctid;
      *ctid = static_cast<uint32_t>(*pid);
    }
  }

  Status<uint64_t> as = CloneCurrentAddressSpace(exclude);

  if (ctid) *ctid = saved_ctid;

  if (!as) {
    ReleasePid(*pid, get_pgid(), get_sid());
    return MakeError(as);
  }

  // Each fork is a chance to notice what has diverged since the last one: the
  // audit compares every live address space against this one.
  // The probe first, so a single fork is enough to test the audit: it creates
  // the divergence that the audit immediately afterwards has to find.
  if (unlikely(GetCfg().debug_frozen_probe())) FrozenViolationProbe();
  if (unlikely(GetCfg().debug_arena_probe())) ArenaProbe();
  if (unlikely(GetCfg().debug_as_audit())) AuditAddressSpaceCoherence();

  std::shared_ptr<MemoryMap> mm = MemoryMap::Fork(*mem_map_, *as);

  rt::ThreadWaker no_waker;  // not a vfork: the parent keeps running
  auto p = std::make_shared<Process>(*pid, std::move(mm), file_tbl_,
                                     std::move(no_waker), shared_from_this(),
                                     get_pgid(), get_fs(), get_sid());
  p->get_signal_table().CopyFrom(signal_tbl_);
  p->limits_.CopyFrom(limits_);
  p->xstate_ = 0;

  rt::SpinGuard g(shared_sig_q_);
  child_procs_.push_back(p);
  return p;
}

Status<Thread *> Process::CreateThreadMain(const Thread &oldth) {
  thread_t *th = thread_create(nullptr, 0);
  if (!th) return MakeError(ENOMEM);

  Thread *tstate = reinterpret_cast<Thread *>(th->junction_tstate_buf);
  new (tstate) Thread(shared_from_this(), get_pid(), oldth);
  th->junction_thread = true;
  th->has_fsbase = true;
  th->fsbase = 0;
  thread_map_[get_pid()] = tstate;
  return tstate;
}

Status<Thread *> Process::CreateThread(const Thread &oldth) {
  thread_t *th = thread_create(nullptr, 0);
  if (unlikely(!th)) return MakeError(ENOMEM);

  Status<pid_t> tid = AllocPid();
  if (!tid) {
    thread_free(th);
    return MakeError(tid);
  }

  Thread *tstate = reinterpret_cast<Thread *>(th->junction_tstate_buf);

  {
    rt::UniqueLock g(child_thread_lock_);
    if (unlikely(exited())) {
      g.Unlock();
      ReleasePid(*tid);
      thread_free(th);
      return MakeError(1);
    }
    new (tstate) Thread(shared_from_this(), *tid, oldth);
    th->junction_thread = true;
    thread_map_[*tid] = tstate;
  }

  return tstate;
}

int WaitStateToSi(unsigned int state) {
  switch (state) {
      // TODO: differentiate killed and exited
    case kWaitableExited:
      return CLD_EXITED;
    case kWaitableStopped:
      return CLD_STOPPED;
    case kWaitableContinued:
      return CLD_CONTINUED;
    default:
      BUG();
  }
}

void Process::FillWaitInfo(siginfo_t &info) const {
  info.si_signo = SIGCHLD;
  info.si_pid = get_pid();
  info.si_uid = 0;
  if (wait_state_ == kWaitableExited && term_signal_) {
    info.si_status = term_signal_;
    info.si_code = CLD_KILLED;
    return;
  }
  info.si_status = wait_status_;
  info.si_code = WaitStateToSi(wait_state_);
}

int Process::GetWaitStatus() const {
  switch (wait_state_) {
    case kWaitableExited:
      // A process killed by a signal is reported as such, not as an exit code.
      if (term_signal_) return __W_EXITCODE(0, term_signal_);
      return __W_EXITCODE(wait_status_, 0);
    case kWaitableStopped:
      return __W_STOPCODE(SIGSTOP);
    case kWaitableContinued:
      return __W_CONTINUED;
    default:
      BUG();
  }
}

void Process::ReapChild(Process *child) {
  assert(shared_sig_q_.IsHeld());

  if (child->wait_state_ != kWaitableExited) {
    child->wait_state_ = kNotWaitable;
    return;
  }

  child->parent_.reset();
  std::erase_if(child_procs_, [child](std::shared_ptr<Process> &p) {
    return p.get() == child;
  });
}

void Process::NotifyParentWait(unsigned int state, int status) {
  if (!parent_) return;

  rt::UniqueLock<rt::Spin> lock(parent_->shared_sig_q_);
  wait_status_ = status;
  wait_state_ = state;

  siginfo_t sig;
  FillWaitInfo(sig);
  // forward signal
  parent_->SignalLocked(std::move(lock), sig);
}

void Process::KillThreadsAndWait() {
  bool in_proc = IsJunctionThread() && this == &myproc();
  rt::SpinGuard g(child_thread_lock_);

  // Kill any threads besides this one.
  for (const auto &[pid, th] : thread_map_)
    if (!in_proc || pid != mythread().get_tid()) th->Kill();

  size_t wait_for = in_proc ? 1 : 0;
  // Wait for other threads to exit.
  rt::Wait(child_thread_lock_, exec_waker_,
           [&] { return thread_map_.size() == wait_for; });
}

void Process::DoExit(int status) {
  // notify threads
  {
    rt::SpinGuard g(child_thread_lock_);
    if (exited_ || exec_waker_) return;

    xstate_ = status;
    store_release(&exited_, true);
    stopped_threads_.WakeAll();

    // Kill any threads besides this one.
    for (const auto &[pid, th] : thread_map_)
      if (pid != mythread().get_tid()) th->Kill();

    // Wait for other threads to exit.
    rt::Wait(child_thread_lock_, exec_waker_,
             [this] { return thread_map_.size() == 1; });
  }

  if (status != 0)
    LOG(INFO) << "proc: pid " << get_pid() << " exiting with code " << status;

  vfork_waker_.Wake();
  NotifyParentWait(kWaitableExited, status);

  if (is_session_leader()) {
    /* send sighup to procs in the foreground process group */
  }
}

Status<Process *> Process::FindWaitableProcess(idtype_t idtype, id_t id,
                                               unsigned int wait_flags) {
  assert(shared_sig_q_.IsHeld());

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 36)
  if (idtype == P_PIDFD) return MakeError(EINVAL);
#endif
  if (idtype == P_PGID && id == 0) id = get_pgid();

  bool has_candidates = false;

  for (auto &p : child_procs_) {
    // Check filter conditions
    if (idtype == P_PID && p->get_pid() != static_cast<pid_t>(id)) continue;
    if (idtype == P_PGID && p->get_pgid() != static_cast<pid_t>(id)) continue;

    has_candidates = true;
    if (p->get_wait_state() & wait_flags) return p.get();
  }

  if (!has_candidates) return MakeError(ECHILD);
  return nullptr;
}

Status<pid_t> Process::DoWait(idtype_t idtype, id_t id, int options,
                              siginfo_t *infop, int *wstatus) {
  unsigned int wait_state_flags = WEXITED;
  wait_state_flags |= options & (WCONTINUED | WSTOPPED);

  bool nonblocking = options & WNOHANG;
  bool dont_reap = options & WNOWAIT;

  Status<Process *> tmp;

  rt::UniqueLock<rt::Spin> g(shared_sig_q_);

  if (!nonblocking) {
    WaitInterruptible(shared_sig_q_, child_waiters_, [&, this] {
      tmp = FindWaitableProcess(idtype, id, wait_state_flags);
      return !tmp || *tmp;
    });

    // check one more time, we may have been woken by a SIGCHLD
    if (!tmp || !*tmp) tmp = FindWaitableProcess(idtype, id, wait_state_flags);
    if (!tmp) return MakeError(tmp);
    if (!*tmp) return MakeError(ERESTARTSYS);
  } else {
    tmp = FindWaitableProcess(idtype, id, wait_state_flags);
    if (!tmp) return MakeError(tmp);
    if (!*tmp) return MakeError(EAGAIN);
  }

  Process *p = *tmp;
  siginfo_t info;
  int status = p->GetWaitStatus();
  if (infop) p->FillWaitInfo(info);
  pid_t pid = p->get_pid();
  if (!dont_reap) ReapChild(p);
  g.Unlock();

  // Write to the caller's memory only after dropping the lock. A fault here
  // would otherwise be taken with preemption disabled, which is not
  // recoverable -- and a bad pointer is the caller's mistake, not a reason to
  // take down the container.
  // Linux reaps the child and only then copies the status out, so a bad
  // pointer costs the caller the exit status entirely: the child is already
  // gone and a second wait reports ECHILD. Match that, wart and all.
  MemoryMap &mm = get_mem_map();
  if (infop) {
    if (!mm.CheckAccess(infop, sizeof(*infop), PROT_WRITE))
      return MakeError(EFAULT);
    *infop = info;
  }
  if (wstatus) {
    if (!mm.CheckAccess(wstatus, sizeof(*wstatus), PROT_WRITE))
      return MakeError(EFAULT);
    *wstatus = status;
  }
  return pid;
}

void Process::DumpAllThreads(const char *why) {
  LOG(ERR) << "==== thread dump: " << why << " ====";
  // An anchor for resolving the rips below. The binary is PIE, so:
  //   bias = this value - (nm junction_run | grep DumpAllThreads)
  //   addr2line -e junction_run  <rip - bias>
  LOG(ERR) << "text anchor: DumpAllThreads=0x" << std::hex
           << reinterpret_cast<uintptr_t>(&Process::DumpAllThreads) << std::dec;

  ForEachProcess([](Process &p) {
    // ForEachThread runs its callback under a spin lock with preemption
    // disabled, so collect first and print afterwards -- LOG() allocates.
    struct Snap {
      pid_t tid;
      int syscall;
      // The interruptible-wait refcount (inc/runtime/interruptible_wait.h):
      // 65 = parked and armed, 66 = a signal also arrived, 0 = woken normally.
      // A parked thread with no futex entry and an unexpected value here is
      // the accounting bug, not a lost wakeup.
      int interrupt_state;
      bool in_kernel, ready, running, in_syscall, linked;
      uint64_t rip, rsp;
    };
    constexpr size_t kMax = 64;
    Snap snaps[kMax];
    size_t n = 0;

    p.ForEachThread([&](Thread &th) {
      if (n == kMax) return;
      thread_t *ct = th.GetCaladanThread();
      snaps[n++] = Snap{th.get_tid(), th.get_cur_syscall(),
                        static_cast<int>(atomic8_read(&ct->interrupt_state)),
                        th.in_kernel(),
                        ct->thread_ready,
                        ct->thread_running,
                        ct->in_syscall,
                        ct->link_armed,
                        ct->tf.rip,
                        ct->tf.rsp};
    });

    // A poor-man's backtrace for each blocked thread.
    //
    // The blocked rip only ever says "parked in WaitInterruptible"; what
    // matters is who called it, and -O3 leaves no frame pointer to walk. So
    // scan the thread's stack for words that look like return addresses into
    // our own text, which is enough to name the caller. Reading a parked
    // thread's stack is safe: it is live memory in the shared stack pool and
    // nothing is running on it.
    const uintptr_t anchor =
        reinterpret_cast<uintptr_t>(&Process::DumpAllThreads);
    constexpr uintptr_t kTextWindow = 64UL << 20;
    for (size_t i = 0; i < n; i++) {
      const uint64_t *sp = reinterpret_cast<const uint64_t *>(snaps[i].rsp);
      if (!sp) continue;
      size_t found = 0;
      for (size_t w = 0; w < 512 && found < 6; w++) {
        uint64_t v = sp[w];
        if (v > anchor - kTextWindow && v < anchor + kTextWindow)
          LOG(ERR) << "  tid " << snaps[i].tid << " stack[" << w << "] = 0x"
                   << std::hex << v << std::dec << "  (+0x" << std::hex
                   << (v - (anchor - 0x19b930)) << std::dec << ")";
        if (v > anchor - kTextWindow && v < anchor + kTextWindow) found++;
      }
    }

    LOG(ERR) << "pid " << p.get_pid() << ": " << n << " thread(s)"
             << ", as=" << p.get_mem_map().get_as_handle()
             << ", wait_state=" << p.get_wait_state()
             << (p.exited() ? " [exited]" : "");
    for (size_t i = 0; i < n; i++) {
      const Snap &s = snaps[i];
      // ready/running are Caladan's view: a thread that is neither, and is
      // linked onto a wait queue, is blocked. That is the state a lost wakeup
      // leaves behind, and rip says on what.
      LOG(ERR) << "  tid " << s.tid << " " << (s.running ? "RUNNING" : "")
               << (s.ready ? "READY " : "")
               << (!s.ready && !s.running ? "BLOCKED" : "")
               << (s.linked ? " queued" : "")
               << (s.in_syscall ? " in_syscall" : "")
               << (s.in_kernel ? " in_kernel" : "") << " istate=" << s.interrupt_state
               << " syscall=" << s.syscall
               << " rip=0x" << std::hex
               << s.rip << " rsp=0x" << s.rsp << std::dec;
    }
  });

  // Futex waiters. Reading *key is a guest-memory read from whatever address
  // space this thread is bound to, so the value is only meaningful for keys
  // belonging to that address space -- print it anyway, flagged, because a
  // wrong-looking value is itself the finding.
  LOG(ERR) << "futex waiters (as=" << GetActiveAddressSpace() << "):";
  size_t nw = 0;
  FutexTable::GetFutexTable().ForEachWaiter(
      [&](uint32_t *key, uint32_t bitset) {
        if (nw++ >= 16) return;
        LOG(ERR) << "  key=0x" << std::hex << reinterpret_cast<uintptr_t>(key)
                 << " bitset=0x" << bitset << std::dec;
      });
  LOG(ERR) << "  " << nw << " waiter(s) total";

  LOG(ERR) << "==== end thread dump ====";
}

void Process::ThreadStopWait() {
  rt::SpinGuard g(child_thread_lock_);
  if (!is_stopped()) return;

  if (++stopped_count_ == thread_map_.size()) {
    NotifyParentWait(kWaitableStopped);

    // Also notify waiters that all threads have stopped
    stopped_threads_.WakeAll();
  }

  rt::Wait(child_thread_lock_, stopped_threads_,
           [&]() { return !is_stopped() || exited_; });
  stopped_count_--;
}

void Thread::StopWait(int rax) {
  // Flag should be set on entry.
  assert(rt::GetInterruptibleStatus(GetCaladanThread()) !=
         rt::InterruptibleStatus::kNone);
  assert(!check_prepared(GetCaladanThread()));
  assert(in_kernel());
  cold().stopped_rax_ = rax;
  myproc().ThreadStopWait();
}

long usys_wait4(pid_t pid, int *wstatus, int options, struct rusage *ru) {
  const auto &[idtype, id] = PidtoId(pid);
  Status<pid_t> ret = myproc().DoWait(idtype, id, options, nullptr, wstatus);
  if (!ret) return MakeCError(ret);
  if (ru) {
    if (!myproc().get_mem_map().CheckAccess(ru, sizeof(*ru), PROT_WRITE))
      return -EFAULT;
    memset(ru, 0, sizeof(*ru));
  }
  return *ret;
}

long usys_waitid(int which, pid_t pid, siginfo_t *infop, int options,
                 struct rusage *ru) {
  Status<pid_t> ret = myproc().DoWait(static_cast<idtype_t>(which), pid,
                                      options, infop, nullptr);
  if (!ret) return MakeCError(ret);
  if (ru) {
    if (!myproc().get_mem_map().CheckAccess(ru, sizeof(*ru), PROT_WRITE))
      return -EFAULT;
    memset(ru, 0, sizeof(*ru));
  }
  return 0;
}

long usys_getppid() { return myproc().get_ppid(); }

long usys_getpid() {
  // Diagnostic probe: allocate through Junction's own glibc while a guest is
  // the current address space, and see where the mapping ends up. glibc has no
  // brk here (the seccomp handler returns 0 for it), so an allocation past its
  // pre-grown cushion goes to mmap, which seccomp traps and executes natively
  // -- against whichever address space this core is bound to.
  static uintptr_t debug_libos_ptr = 0;
  static uint64_t debug_libos_as = 0;

  if (unlikely(GetCfg().debug_libos_escape()) && myproc().get_pid() != 1) {
    static std::atomic_bool escape_done{false};
    bool e = false;
    if (escape_done.compare_exchange_strong(e, true)) RunLibOSEscapeProbe();
  }

  // Deliberately provoke the case the memfd arena exists to fix: the LibOS
  // allocating through its own glibc while a *forked* guest's address space is
  // the one loaded on this core. Fires once, from a child, so the active
  // address space is a clone rather than the root.
  if (unlikely(GetCfg().debug_libos_alloc_mb() > 0) &&
      myproc().get_pid() != 1) {
    static std::atomic_bool done{false};
    bool expected = false;
    if (done.compare_exchange_strong(expected, true)) {
      size_t len = GetCfg().debug_libos_alloc_mb() << 20;
      uint64_t guest_fs = GetFSBase();
      LOG(INFO) << "debug: guest pid " << myproc().get_pid() << " as="
                << GetActiveAddressSpace() << " guest fsbase=0x" << std::hex
                << guest_fs << std::dec
                << " preempt_enabled=" << (preempt_enabled() ? 1 : 0)
                << " (must work with preemption ENABLED)";

      // Junction's glibc keeps its arena pointer and tcache in TLS, and a guest
      // syscall runs on the *guest's* TCB -- calling malloc with this fsbase
      // reads a foreign glibc's TLS layout and dies. Restore the LibOS fsbase
      // so we are measuring the address space, not the TLS.
      SetFSBase(perthread_read(runtime_fsbase));

      // No preempt_disable() here on purpose. The trap handler now classifies
      // on the trapping rip, so this must work with preemption enabled -- which
      // is the state every guest syscall handler runs in.
      void *p = std::malloc(len);
      if (p) std::memset(p, 0xab, len);

      uintptr_t pa = reinterpret_cast<uintptr_t>(p);
      SetFSBase(guest_fs);

      LOG(INFO) << "debug: LibOS malloc(" << (len >> 20) << " MB) -> 0x"
                << std::hex << pa << std::dec << " ["
                << (!pa ? "failed"
                        : pa < kVirtualAreaMax
                              ? "GUEST-RANGE: owned by a guest, invisible to "
                                "every other address space"
                              : "libos-range")
                << "] in address space " << GetActiveAddressSpace();
      debug_libos_ptr = pa;
      debug_libos_as = GetActiveAddressSpace();
    }
  }

  // ...and, from a different address space, ask the host whether that mapping
  // exists here. msync() reports ENOMEM for an unmapped range without faulting,
  // so this answers the question the crash would otherwise answer for us.
  if (unlikely(GetCfg().debug_libos_alloc_mb() > 0) && debug_libos_ptr != 0 &&
      GetActiveAddressSpace() != debug_libos_as) {
    uintptr_t pa = debug_libos_ptr;
    debug_libos_ptr = 0;
    // Control: LibOS text, mapped long before any fork, must be visible from
    // every address space. If this said ABSENT the probe would be measuring
    // its own bug rather than Junction's.
    uintptr_t control = reinterpret_cast<uintptr_t>(&usys_getpid);
    // Ask the host which mappings this address space actually has. Going
    // through /proc/self/maps rather than msync() because the seccomp filter
    // answers ENOSYS for msync, and because the maps line also shows what the
    // range became.
    char line[256];
    auto lookup = [&](uintptr_t want) -> bool {
    bool present = false;
    int n = 0;
    int fd = ksys_open("/proc/thread-self/maps", O_RDONLY, 0);
    if (fd >= 0) {
      char buf[4096];
      ssize_t got;
      off_t pos = 0;
      while (!present &&
             (got = ksys_pread(fd, buf, sizeof(buf), pos)) > 0) {
        pos += got;
        for (ssize_t i = 0; i < got; i++) {
          if (buf[i] != '\n') {
            if (n < static_cast<int>(sizeof(line)) - 1) line[n++] = buf[i];
            continue;
          }
          line[n] = '\0';
          uintptr_t lo = 0, hi = 0;
          int j = 0;
          auto hex = [&](uintptr_t &out) {
            out = 0;
            while (line[j]) {
              char c = line[j];
              int d;
              if (c >= '0' && c <= '9') d = c - '0';
              else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
              else break;
              out = (out << 4) | static_cast<uintptr_t>(d);
              j++;
            }
          };
          hex(lo);
          if (line[j] == '-') { j++; hex(hi); }
          // Readable, not merely covered: the arena's PROT_NONE reservation
          // spans its whole slot in every address space, so "some VMA covers
          // it" is always true there and proves nothing.
          if (lo && want >= lo && want < hi && line[j] == ' ' &&
              line[j + 1] == 'r') {
            present = true;
            break;
          }
          n = 0;
        }
      }
      ksys_close(fd);
    }
    return present;
    };
    bool ctl = lookup(control);
    bool present = lookup(pa);
    LOG(INFO) << "debug: address space " << GetActiveAddressSpace()
              << " looking for the LibOS mapping made in address space "
              << debug_libos_as << " at 0x" << std::hex << pa << std::dec
              << ": "
              << (present ? "PRESENT"
                          : "ABSENT -- the LibOS allocated memory that this "
                            "address space cannot see");
    // Now actually use it, the way any LibOS code holding a pointer into its
    // own heap would. This is the crash: glibc's free lists are shared LibOS
    // state, so a chunk handed out here is one another address space mapped.
    if (!present) {
      // Absent from the map is what a lazily repaired arena range looks like
      // until it is touched. Touch it: the read faults, the fault is repaired
      // from the shadow map, and the bytes must be the ones the other address
      // space wrote (0xab) -- that is the proof, not the absence of a crash.
      LOG(INFO) << "debug: absent from this address space's map; touching it";
      volatile unsigned char *b = reinterpret_cast<volatile unsigned char *>(pa);
      unsigned char seen = b[0];
      b[0] = 0x5a;
      if (seen == 0xab)
        LOG(INFO) << "debug: REPAIRED ON TOUCH (read 0xab, written in address "
                     "space "
                  << debug_libos_as << ")";
      else
        LOG(INFO) << "debug: touch succeeded but read 0x" << std::hex
                  << static_cast<int>(seen) << std::dec
                  << ", not the 0xab written in address space "
                  << debug_libos_as << " -- WRONG BYTES";
    }
    LOG(INFO) << "debug:   control (LibOS text at 0x" << std::hex << control
              << std::dec << ", mapped before the fork): "
              << (ctl ? "PRESENT, as expected" : "ABSENT -- probe is broken");
  }
  return myproc().get_pid();
}

long usys_gettid() { return mythread().get_tid(); }

long usys_getpgrp() { return myproc().get_pgid(); }

long usys_setpgid(pid_t pid, pid_t pgid) {
  // TODO: check that pgid is a valid process group and that pgid's sid matches.
  Status<void> ret;

  if (pgid == 0) pgid = myproc().get_pgid();

  if (pid == 0 || pid == myproc().get_pid()) {
    ret = myproc().JoinProcessGroup(pgid);
  } else {
    std::shared_ptr<Process> proc = Process::Find(pid);
    if (!proc) return -ESRCH;
    ret = proc->JoinProcessGroup(pgid);
  }

  if (unlikely(ret)) return MakeCError(ret);
  return 0;
}

long usys_getpgid(pid_t pid) {
  if (pid == 0 || pid == myproc().get_pid()) return myproc().get_pgid();
  std::shared_ptr<Process> proc = Process::Find(pid);
  if (!proc) return -ESRCH;
  return proc->get_pgid();
}

long usys_getsid(pid_t pid) {
  if (pid == 0 || pid == myproc().get_pid()) return myproc().get_sid();
  std::shared_ptr<Process> proc = Process::Find(pid);
  if (!proc) return -ESRCH;
  return proc->get_sid();
}

long usys_setsid() {
  Process &p = myproc();
  if (p.is_session_leader()) return 0;
  if (p.is_process_group_leader()) return -EPERM;
  Status<pid_t> ret = p.BecomeSessionLeader();
  if (unlikely(!ret)) return MakeCError(ret);
  return *ret;
}

long usys_arch_prctl(int code, unsigned long addr) {
  // TODO: supporting Intel AMX requires requesting the feature from the kernel.
  if (code != ARCH_SET_FS) return -EINVAL;
  thread_set_fsbase(thread_self(), addr);
  return 0;
}

long usys_set_tid_address(int *tidptr) {
  Thread &tstate = mythread();
  tstate.set_child_tid(reinterpret_cast<uint32_t *>(tidptr));
  return tstate.get_tid();
}

long usys_fork() {
  clone_args cl_args;
  memset(&cl_args, 0, sizeof(cl_args));

  // A bare fork() shares nothing with its parent and reports the child's death
  // with SIGCHLD. glibc has not used this system call since it started needing
  // CLONE_CHILD_SETTID, but programs still call it directly.
  cl_args.flags = 0;
  cl_args.exit_signal = SIGCHLD;

  long ret = DoClone(&cl_args, mythread().GetSyscallFrame().GetRsp());
  if (unlikely(GetCfg().strace_enabled())) LogSyscallDirect(ret, "fork");
  return ret;
}

long usys_vfork() {
  clone_args cl_args;
  memset(&cl_args, 0, sizeof(cl_args));

  cl_args.flags = kVforkRequiredFlags;

  long ret = DoClone(&cl_args, mythread().GetSyscallFrame().GetRsp());
  if (unlikely(GetCfg().strace_enabled() && ret < 0))
    LogSyscallDirect(ret, "vfork");

  return ret;
}

long usys_clone3(struct clone_args *cl_args, size_t size) {
  long ret;
  if (unlikely(!cl_args->stack))
    ret = -EINVAL;
  else
    ret = DoClone(cl_args, cl_args->stack + cl_args->stack_size);

  if (unlikely(GetCfg().strace_enabled()))
    LogSyscallDirect(ret, "clone3",
                     static_cast<strace::CloneFlag>(cl_args->flags),
                     reinterpret_cast<void *>(cl_args->stack),
                     reinterpret_cast<void *>(cl_args->parent_tid),
                     reinterpret_cast<void *>(cl_args->child_tid),
                     reinterpret_cast<void *>(cl_args->tls));

  return ret;
}

long usys_clone(unsigned long clone_flags, unsigned long newsp,
                uintptr_t parent_tidptr, uintptr_t child_tidptr,
                unsigned long tls) {
  clone_args cl_args;
  memset(&cl_args, 0, sizeof(cl_args));

  if (!newsp) newsp = mythread().GetSyscallFrame().GetRsp();

  cl_args.flags = clone_flags;
  cl_args.child_tid = child_tidptr;
  cl_args.parent_tid = parent_tidptr;
  cl_args.tls = tls;

  long ret = DoClone(&cl_args, newsp);
  if (unlikely(GetCfg().strace_enabled())) {
    LogSyscallDirect(ret, "clone", static_cast<strace::CloneFlag>(clone_flags),
                     reinterpret_cast<void *>(newsp),
                     reinterpret_cast<void *>(parent_tidptr),
                     reinterpret_cast<void *>(child_tidptr),
                     reinterpret_cast<void *>(tls));
  }
  return ret;
}

[[noreturn]] void FinishExit(int status) {
  Thread *tptr = &mythread();
  tptr->set_xstate(status);
  // Clear rseq.
  tptr->get_rseq().set(nullptr, 0, 0);
  rt::Preempt::Lock();
  if (--tptr->cold().ref_count_ <= 0) {
    assert(tptr->cold().ref_count_ == 0);
    rt::Preempt::Unlock();
    RunOnSyscallStack([tptr]() {
      tptr->~Thread();
      rt::Exit();
    });
  }

  // A reference holder will clean up this thread.
  rt::Preempt::UnlockAndPark();
  std::unreachable();
}

// For debugging in gdb.
__noinline Thread *gdb_mythread() {
  return &Thread::fromCaladanThread(thread_self());
}
__noinline Process *gdb_myproc() { return &mythread().get_process(); }

[[noreturn]] void usys_exit(int status) {
  if (unlikely(GetCfg().strace_enabled())) LogSyscallDirect("exit", status);
  FinishExit(status);
}

void usys_exit_group(int status) {
  if (unlikely(GetCfg().strace_enabled()))
    LogSyscallDirect("exit_group", status);
  myproc().DoExit(status);
  FinishExit(status);
}

void RseqState::_fixup(Thread &th, Trapframe *tf) {
  struct rseq_cs *cs = reinterpret_cast<struct rseq_cs *>(user_rs_->rseq_cs);
  assert(cs);

  Process &proc = th.get_process();

  if (unlikely(proc.get_mem_map().TraceEnabled())) {
    // We are tracing AND there was a reschedule during an rseq critical
    // section. Make sure that the tracer is notified of the CS memory and fix
    // the permissions. This code may be called from non-Junction thread stacks.
    bool new_hit =
        proc.get_mem_map().RecordHit(cs, sizeof(*cs), Time::Now(), PROT_WRITE);
    if (new_hit) {
      std::byte *start = PageAlignDown(cs);
      std::byte *end = PageAlign(cs + 1);
      Status<void> ret =
          KernelMProtect(start, end - start, PROT_READ | PROT_WRITE);
      if (unlikely(!ret))
        LOG(WARN) << "tracer could not mprotect rseq cs" << ret.error();
    }
  }

  if (!tf) tf = &th.GetTrapframe();
  assert(tf == &th.GetTrapframe());

  uintptr_t ip = tf->GetRip();
  if (ip - cs->start_ip < cs->post_commit_offset) tf->SetRip(cs->abort_ip);

  user_rs_->rseq_cs = 0;
}

long usys_rseq(struct rseq *rseq, uint32_t rseq_len, int flags, uint32_t sig) {
  RseqState &rsstate = mythread().get_rseq();
  if (flags & RSEQ_FLAG_UNREGISTER) {
    if (flags & ~RSEQ_FLAG_UNREGISTER) return -EINVAL;
    if (!rsstate) return -EINVAL;
    if (Status<void> ret = rsstate.matches(rseq, rseq_len, sig); !ret)
      return MakeCError(ret);
    rsstate.reset();
    return 0;
  }

  if (rsstate) {
    if (Status<void> ret = rsstate.matches(rseq, rseq_len, sig); !ret)
      return MakeCError(ret);
    return -EBUSY;
  }

  if (rseq_len < kRseqSize ||
      reinterpret_cast<uintptr_t>(rseq) % kRseqSize != 0)
    return -EINVAL;
  rsstate.set(rseq, rseq_len, sig);
  return 0;
}

}  // namespace junction
