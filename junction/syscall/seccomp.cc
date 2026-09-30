#include "junction/syscall/seccomp.h"

#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <sys/uio.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
#include <base/signal.h>
}

#include "junction/bindings/log.h"
#include "junction/junction.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/memtrace.h"
#include "junction/kernel/mm.h"
#include "junction/kernel/arena.h"
#include "junction/kernel/as.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/sigframe.h"
#include "junction/kernel/trapframe.h"
#include "junction/syscall/entry.h"
#include "junction/syscall/seccomp_bpf.h"
#include "junction/syscall/syscall.h"
#include "junction/syscall/systbl.h"

namespace junction {

// Filter for syscalls from the caladan runtime.
static struct sock_filter caladan_filter[] = {
    ALLOW_CALADAN_SYSCALL(ioctl),      ALLOW_CALADAN_SYSCALL(mmap),
    ALLOW_CALADAN_SYSCALL(madvise),    ALLOW_CALADAN_SYSCALL(mprotect),
    ALLOW_CALADAN_SYSCALL(exit_group), ALLOW_CALADAN_SYSCALL(pwritev2),
    ALLOW_CALADAN_SYSCALL(writev)};

// Syscalls needed to manipulate the host fs.
static struct sock_filter writeable_linux_fs[] = {
    ALLOW_JUNCTION_SYSCALL(mkdirat),   ALLOW_JUNCTION_SYSCALL(linkat),
    ALLOW_JUNCTION_SYSCALL(unlinkat),  ALLOW_JUNCTION_SYSCALL(renameat2),
    ALLOW_JUNCTION_SYSCALL(symlinkat), ALLOW_JUNCTION_SYSCALL(truncate),
    ALLOW_JUNCTION_SYSCALL(fchmodat),
    ALLOW_JUNCTION_SYSCALL(fchownat)};

// Syscalls needed to query dents/inodes in the host fs at runtime.
static struct sock_filter uncached_linux_fs[] = {
    ALLOW_JUNCTION_SYSCALL(getdents64), ALLOW_JUNCTION_SYSCALL(newfstatat),
    ALLOW_JUNCTION_SYSCALL(readlinkat)};

// Filter that allows all Junction syscalls to passthrough (for debugging).
static struct sock_filter allow_all_junction[] = {ALLOW_ANY_JUNCTION_SYSCALL};

// Filter to enable tgkill().
static struct sock_filter linux_tgkill[] = {ALLOW_JUNCTION_SYSCALL(tgkill)};

// Syscalls used to create, switch between, and tear down address spaces.
// clone() materializes a new address space, pause() is all the resulting clone
// ever executes, and ioctl() drives /dev/junction_as.
static struct sock_filter address_spaces[] = {
    ALLOW_JUNCTION_SYSCALL(ioctl),  ALLOW_JUNCTION_SYSCALL(clone),
    ALLOW_JUNCTION_SYSCALL(pause),  ALLOW_JUNCTION_SYSCALL(kill),
    ALLOW_JUNCTION_SYSCALL(wait4),  ALLOW_JUNCTION_SYSCALL(rt_sigprocmask),
};

static struct sock_filter rtsigreturn[] = {ALLOW_CALADAN_SYSCALL(rt_sigreturn)};

// Final filter that forwards all other system calls to our signal handler.
static struct sock_filter trap[] = {TRAP};

// Filter for core junction functionality.
static struct sock_filter junction_core[] = {
    ALLOW_JUNCTION_SYSCALL(mmap),
    ALLOW_JUNCTION_SYSCALL(munmap),
    ALLOW_JUNCTION_SYSCALL(mprotect),
    ALLOW_JUNCTION_SYSCALL(madvise),
    // Hole punches in the arena's and memfs's memfds. They go through the fd
    // because the address space that frees a range need not have it mapped.
    ALLOW_JUNCTION_SYSCALL(fallocate),
    ALLOW_JUNCTION_SYSCALL(openat),
    ALLOW_JUNCTION_SYSCALL(close),
    ALLOW_JUNCTION_SYSCALL(preadv2),
    ALLOW_JUNCTION_SYSCALL(pread64),
    ALLOW_JUNCTION_SYSCALL(exit_group),
#ifdef FUNCTION_PROFILING
    ALLOW_JUNCTION_SYSCALL(perf_event_open),
#endif
};

constexpr size_t filterMax =
    sizeof(caladan_filter) + sizeof(writeable_linux_fs) +
    sizeof(uncached_linux_fs) + sizeof(allow_all_junction) +
    sizeof(linux_tgkill) + sizeof(trap) + sizeof(junction_core) +
    sizeof(rtsigreturn) + sizeof(address_spaces);

/* Source: https://outflux.net/teach-seccomp/step-3/example.c
 */
Status<void> _install_seccomp_filter() {
  unsigned char filter[filterMax];
  size_t pos = 0;

  auto addFilter = [&pos, &filter](void *newf, size_t size) {
    if (pos + size > filterMax)
      throw std::runtime_error("seccomp: not enough space in filter");
    memcpy(&filter[pos], newf, size);
    pos += size;
  };

  // Add caladan filters.
  addFilter(caladan_filter, sizeof(caladan_filter));

#ifdef PERMISSIVE_SECCOMP
  // Allow any Junction system call.
  addFilter(allow_all_junction, sizeof(allow_all_junction));
#else

  // Add Junction core filters.
  addFilter(junction_core, sizeof(junction_core));

  // Allow the address-space operations fork() depends on.
  if (MultiAddressSpaceEnabled())
    addFilter(address_spaces, sizeof(address_spaces));

#ifdef WRITEABLE_LINUX_FS
  // Allow system calls to modify host fs.
  addFilter(writeable_linux_fs, sizeof(writeable_linux_fs));
#endif

  // Add system calls needed to query dirents/inode stats.
  if (!GetCfg().cache_linux_fs())
    addFilter(uncached_linux_fs, sizeof(uncached_linux_fs));

  // Allow tgkill if uintr is not available.
  if (!uintr_enabled) {
    addFilter(rtsigreturn, sizeof(rtsigreturn));
    addFilter(linux_tgkill, sizeof(linux_tgkill));
  }

#endif

  // Finally, trap remaining calls.
  addFilter(trap, sizeof(trap));

  struct sock_fprog prog = {
      .len = (unsigned short)(pos / sizeof(struct sock_filter)),
      .filter = (struct sock_filter *)filter,
  };

  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
    perror("prctl(NO_NEW_PRIVS)");
    if (errno == EINVAL) {
      fprintf(stderr, "SECCOMP_FILTER is not available. :(\n");
    }
    return MakeError(-errno);
  }

  int rv = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                   SECCOMP_FILTER_FLAG_TSYNC, &prog);
  if (rv) {
    perror("syscall(SECCOMP_SET_MODE_FILTER)");
    return MakeError(rv);
  }

  return {};
}

void log_syscall_msg(const char *msg_needed, long sysn) {
  char buf[128], *pos;
  memcpy(buf, msg_needed, strlen(msg_needed));
  pos = buf + strlen(msg_needed);
  *pos++ = ' ';
  *pos++ = '(';
  size_t slen = strlen(syscall_names[sysn]);
  // This will likely cause a segfault instead of printing
  BUG_ON(pos - buf + slen + 2 > sizeof(buf));
  memcpy(pos, syscall_names[sysn], slen);
  pos += slen;
  *pos++ = ')';
  *pos++ = '\n';
  ksys_write(STDOUT_FILENO, buf, pos - buf);
}

namespace {

// Records a memory-mapping syscall that seccomp trapped out of Junction's own
// libc. Everything else is left alone; the point is the memory operations.
void TraceTrappedMemSyscall(k_ucontext *ctx, long sysn, long res) {
  const char *op = nullptr;
  switch (sysn) {
    case __NR_mmap: op = "mmap"; break;
    case __NR_munmap: op = "munmap"; break;
    case __NR_mprotect: op = "mprotect"; break;
    case __NR_madvise: op = "madvise"; break;
    case __NR_mremap: op = "mremap"; break;
    case __NR_brk: op = "brk"; break;
    default: return;
  }
  MemTraceMap(MemTraceSource::kJunctionLibc, op,
              static_cast<uintptr_t>(ctx->uc_mcontext.rdi),
              static_cast<size_t>(ctx->uc_mcontext.rsi),
              static_cast<int>(ctx->uc_mcontext.rdx),
              static_cast<int>(ctx->uc_mcontext.r10),
              static_cast<int>(static_cast<long>(ctx->uc_mcontext.r8)),
              static_cast<int64_t>(ctx->uc_mcontext.r9),
              static_cast<uintptr_t>(ctx->uc_mcontext.rip), res);
}

}  // namespace

extern "C" void syscall_trap_handler(int nr, siginfo_t *info,
                                     void *void_context) {
  k_ucontext *ctx = reinterpret_cast<k_ucontext *>(void_context);

  if (unlikely(info->si_code != SYS_SECCOMP)) {
    log_syscall_msg("Unexpected signal delivered to syscall handler", 0);
    syscall_exit(-1);
  }

  if (unlikely(!ctx)) {
    log_syscall_msg("Missing context in syscall handler", 0);
    syscall_exit(-1);
  }

  long sysn = static_cast<long>(ctx->uc_mcontext.rax);

  // Is this Junction's own glibc, or a guest's?
  //
  // This used to test !preempt_enabled(), which is a heuristic and is wrong:
  // LibOS code that runs with preemption enabled -- anything inside a guest
  // syscall handler, for one -- had its glibc syscalls dispatched to the guest
  // syscall table. The observable symptom was Junction's own brk arriving at
  // usys_brk as "Unexpected syscall while in_kernel (brk)".
  //
  // The trapping rip is sound instead, because the two glibcs are separate
  // images on opposite sides of the address-space partition: Junction's is
  // linked into junction_run above kVirtualAreaMax, while a guest's comes from
  // install/lib/libc.so.6 below it, and usys_mmap confines guests there.
  //
  // Not fsbase, which looks equally good and is not: usys_arch_prctl sets it
  // without a range check, so a guest could point it above kVirtualAreaMax and
  // have its own mappings served out of the shared LibOS arena.
  if (unlikely(ctx->uc_mcontext.rip >= kVirtualAreaMax)) {
    // avoid infinitely looping when Junction's glibc makes a blocked syscall

    static bool once;
    if (!once) {
      once = true;
      log_syscall_msg("Trapped a Junction libc internal syscall ", sysn);
    }

    if (unlikely(
            ctx->uc_mcontext.rip >= reinterpret_cast<uint64_t>(&ksys_start) &&
            ctx->uc_mcontext.rip < reinterpret_cast<uint64_t>(&ksys_end))) {
      ctx->uc_mcontext.rax = -ENOSYS;
      log_syscall_msg("blocked syscall (likely from inside Junction's libc): ",
                      sysn);
      // glibc announces why it is about to abort() -- a failed assertion, heap
      // corruption, a terminate() -- with writev(2, ...), and
      // backtrace_symbols_fd() writes a fatal assertion's callers the same
      // way to fd 1. Those lines say what went wrong in the LibOS; let them
      // through.
      if (sysn == __NR_writev && (ctx->uc_mcontext.rdi == STDERR_FILENO ||
                                  ctx->uc_mcontext.rdi == STDOUT_FILENO)) {
        auto *iov = reinterpret_cast<const struct iovec *>(ctx->uc_mcontext.rsi);
        unsigned long cnt = ctx->uc_mcontext.rdx;
        for (unsigned long i = 0; iov && i < cnt && i < 16; i++)
          ksys_write(STDOUT_FILENO, iov[i].iov_base, iov[i].iov_len);
      }
      return;
    }

    // Junction's own glibc waiting on one of its internal locks -- malloc's,
    // mostly: there is one arena (see main()), and LibOS threads on several
    // cores contend for it. The host futex is not an option: glibc's are
    // private futexes, keyed by (mm, address), so a wake from a core bound to
    // another address space would never arrive; and the filter refuses it,
    // which glibc answers with "The futex facility returned an unexpected
    // error code" and abort(). Answer here instead. A wait returns after a
    // short pause -- a spurious wakeup, which the futex contract allows and
    // every glibc waiter re-checks for in a loop -- and a wake has nobody
    // asleep to wake. Contended LibOS locks become spin-waits.
    if (sysn == __NR_futex) {
      int op = static_cast<int>(ctx->uc_mcontext.rsi) & FUTEX_CMD_MASK;
      if (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET) {
        for (int i = 0; i < 64; i++) cpu_relax();
        ctx->uc_mcontext.rax = 0;
      } else if (op == FUTEX_WAKE || op == FUTEX_WAKE_BITSET) {
        ctx->uc_mcontext.rax = 0;
      } else {
        // PI and requeue operations transfer ownership; "0" would be a lie.
        ctx->uc_mcontext.rax = static_cast<unsigned long>(-ENOSYS);
      }
      return;
    }

    // We don't allow the brk system call, set the return value to 0 so
    // Junction's libc uses mmap instead.
    if (sysn == __NR_brk) {
      if (unlikely(MemTraceEnabled())) TraceTrappedMemSyscall(ctx, sysn, 0);
      ctx->uc_mcontext.rax = 0;
      return;
    }

    long arg0 = static_cast<long>(ctx->uc_mcontext.rdi);
    long arg1 = static_cast<long>(ctx->uc_mcontext.rsi);
    long arg2 = static_cast<long>(ctx->uc_mcontext.rdx);
    long arg3 = static_cast<long>(ctx->uc_mcontext.r10);
    long arg4 = static_cast<long>(ctx->uc_mcontext.r8);
    long arg5 = static_cast<long>(ctx->uc_mcontext.r9);

    // This is where Junction's own glibc ends up: its mmap is outside the
    // ksys range, so seccomp traps it here. Executed natively it would land
    // in whichever address space this core is currently bound to, invisible
    // to every other one. So the arena serves it instead: anonymous memory
    // comes out of the arena, and munmap / mprotect / mremap / madvise of
    // arena memory go back to it. What the arena declines (file mappings,
    // shared memory) still runs natively, and the trace shows which.
    long res;
    if (!RouteLibOSMemSyscall(sysn, arg0, arg1, arg2, arg3, arg4, arg5, &res))
      res = ksys_default(arg0, arg1, arg2, arg3, arg4, arg5, sysn);
    if (unlikely(MemTraceEnabled())) TraceTrappedMemSyscall(ctx, sysn, res);

    ctx->uc_mcontext.rax = static_cast<unsigned long>(res);
    return;
  }

  preempt_disable();
  assert_on_runtime_stack();

  if (unlikely(!thread_self())) {
    log_syscall_msg("Unexpected syscall from Caladan", sysn);
    syscall_exit(-1);
  }

  if (unlikely(!IsJunctionThread())) {
    log_syscall_msg("Intercepted syscall originating in junction", sysn);
    syscall_exit(-1);
  }

  if (unlikely(mythread().in_kernel())) {
    log_syscall_msg("Unexpected syscall while in_kernel", sysn);
    syscall_exit(-1);
  }

  LOG_ONCE(WARN) << "Warning: intercepting syscalls with seccomp traps";

  // Special case for rt_sigreturn, we actually don't care about the current
  // signal frame, since rt_sigreturn is doing a full restore of a different
  // signal frame.
  if (sysn == SYS_rt_sigreturn) {
    usys_rt_sigreturn_finish(ctx->uc_mcontext.rsp);
    std::unreachable();
  }

  assert(!IsOnStack(ctx->uc_mcontext.rsp, GetSyscallStack()));

  uint64_t rsp = GetSyscallStackBottom();
  KernelSignalTf &stack_tf = KernelSignalTf(ctx).CloneTo(&rsp);
  k_sigframe &new_frame = stack_tf.GetFrame();

  // stash a copy of rax before the syscall
  new_frame.uc.uc_mcontext.trapno = new_frame.uc.uc_mcontext.rax;
  assert(new_frame.uc.uc_mcontext.trapno >= 0 &&
         new_frame.uc.uc_mcontext.trapno < 4096);

  // stash a pointer to the sigframe in case we need to restart the syscall
  mythread().mark_enter_kernel();
  mythread().SetTrapframe(stack_tf);

  if (GetCfg().zpoline()) {
    RunOnStackAtFromSignalStack(rsp - kRedzoneSize, [] {
      preempt_enable();

      KernelSignalTf &ksig = mythread().CastTfToKernelSig();
      k_sigframe &new_frame = ksig.GetFrame();

      std::byte *insns =
          reinterpret_cast<std::byte *>(new_frame.uc.uc_mcontext.rip - 2);

      const std::byte new_insns[] = {std::byte{0xff}, std::byte{0xd0}};

      myproc().get_mem_map().HotPatchInstructions(new_insns,
                                                  std::span{insns, 2});

      ksig.JmpSyscallStart();
    });
    std::unreachable();
  }

  stack_tf.JmpSyscallStartPreemptEnable();
  std::unreachable();
}

Status<void> _install_signal_handler() {
  struct sigaction act;

  if (sigemptyset(&act.sa_mask) != 0) return MakeError(-errno);

  act.sa_sigaction = &syscall_trap_handler;
  act.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;

  if (uintr_enabled)
    act.sa_restorer = &__kframe_unwind_uiret;
  else
    act.sa_restorer = &syscall_rt_sigreturn;

  if (base_sigaction_full(SIGSYS, &act, NULL) < 0) {
    perror("sigaction");
    return MakeError(-errno);
  }

  return {};
}

Status<void> init_seccomp() {
  // Install signal handlers for syscalls
  Status<void> ret = _install_signal_handler();
  if (!ret) return ret;

  // Install syscall filter.
  return _install_seccomp_filter();
}

}  // namespace junction
