// exec.cc - support for launching elf binaries

extern "C" {
#include <asm/ops.h>
#include <elf.h>
#include <runtime/thread.h>
#include <sys/auxv.h>

#include "lib/caladan/runtime/defs.h"
#include "linux_rseq.h"
}

#include <cstring>

#include "junction/base/arch.h"
#include "junction/base/io.h"
#include "junction/base/string.h"
#include "junction/bindings/log.h"
#include "junction/fs/junction_file.h"
#include "junction/junction.h"
#include "junction/kernel/as.h"
#include "junction/kernel/elf.h"
#include "junction/kernel/exec.h"
#include "junction/kernel/usys.h"
#include "junction/syscall/strace.h"
#include "junction/syscall/syscall.h"
#include "junction/syscall/vdso.h"

#ifndef AT_RSEQ_FEATURE_SIZE
#define AT_RSEQ_FEATURE_SIZE 27 /* rseq supported feature size */
#define AT_RSEQ_ALIGN 28        /* rseq allocation alignment */
#endif

namespace junction {
namespace {

// the number of auxiliary vectors used
inline constexpr size_t kNumAuxVectors = 20;
inline constexpr size_t kMaxInterpFollow = 4;
inline constexpr size_t kStackSize = RUNTIME_STACK_SIZE * 32;

size_t VectorBytes(const std::vector<std::string_view> &vec) {
  size_t len = 0;
  for (auto &v : vec) len += v.size() + 1;
  return len;
}

template <typename T>
constexpr Elf64_auxv_t MakeAuxVec(uint64_t type, T val) {
  return {.a_type{type}, .a_un{.a_val{static_cast<uint64_t>(val)}}};
}

template <typename T>
constexpr Elf64_auxv_t MakeAuxVec(uint64_t type, T *val) {
  return {.a_type{type}, .a_un{.a_val{reinterpret_cast<uint64_t>(val)}}};
}

void SetupAuxVec(std::array<Elf64_auxv_t, kNumAuxVectors> *vec,
                 const char *filename, const elf_data &edata,
                 char *random_ptr) {
  // get hardware capabilities from CPUID
  cpuid_info info;
  cpuid(0x00000001, 0, &info);

  uintptr_t vdso = kVDSOLocation;

  std::get<0>(*vec) = MakeAuxVec(AT_HWCAP, info.edx);
  std::get<1>(*vec) = MakeAuxVec(AT_PAGESZ, kPageSize);
  // TODO(amb): these are kernel clock ticks via sysconf(_SC_CLK_TCK)
  std::get<2>(*vec) = MakeAuxVec(AT_CLKTCK, 1000000);
  std::get<3>(*vec) = MakeAuxVec(AT_PHDR, edata.phdr_addr);
  std::get<4>(*vec) = MakeAuxVec(AT_PHENT, edata.phdr_entsz);
  std::get<5>(*vec) = MakeAuxVec(AT_PHNUM, edata.phdr_num);
  std::get<6>(*vec) = MakeAuxVec(AT_FLAGS, 0);
  std::get<7>(*vec) = MakeAuxVec(AT_ENTRY, edata.entry_addr);
  std::get<8>(*vec) = MakeAuxVec(
      AT_BASE, edata.interp ? edata.interp->map_base : edata.map_base);
  // TODO(jfried): get these from the proc struct
  std::get<9>(*vec) = MakeAuxVec(AT_UID, 1);
  std::get<10>(*vec) = MakeAuxVec(AT_EUID, 1);
  std::get<11>(*vec) = MakeAuxVec(AT_GID, 1);
  std::get<12>(*vec) = MakeAuxVec(AT_EGID, 1);
  std::get<13>(*vec) = MakeAuxVec(AT_SECURE, 0);
  std::get<14>(*vec) = MakeAuxVec(AT_RANDOM, random_ptr);
  std::get<15>(*vec) = MakeAuxVec(AT_EXECFN, filename);
  std::get<16>(*vec) = MakeAuxVec(AT_SYSINFO_EHDR, vdso);
  std::get<17>(*vec) = MakeAuxVec(AT_RSEQ_ALIGN, kRseqSize);
  std::get<18>(*vec) = MakeAuxVec(AT_RSEQ_FEATURE_SIZE, kRseqFeatureSize);
  std::get<19>(*vec) = MakeAuxVec(AT_NULL, 0);  // must be last
}

void SetupStack(uint64_t *sp, const std::vector<std::string_view> &argv,
                const std::vector<std::string_view> &envp, elf_data &edata) {
  size_t len = 0;
  const char *filename;
  uint64_t *arg_ptr;

  // determine the amount of stack we need to reserve
  len += VectorBytes(argv);
  len += VectorBytes(envp);

  char *info_block_ptr = reinterpret_cast<char *>(*sp - len);
  filename = info_block_ptr;

  // Generate random bytes for aux vector.
  char *random_ptr = info_block_ptr - 16;
  Status<size_t> ret = ReadRandom(readable_span(random_ptr, 16));
  if (!ret) LOG(ERR) << "exec: failed to generate random bytes";
  len += 16;

  // The System V AMD64 ABI requires a 16-byte stack
  // alignment. We go with 32-byte to be extra careful.
  len += sizeof(Elf64_auxv_t) * kNumAuxVectors;
  len += (argv.size() + envp.size() + 3) * sizeof(uint64_t);
  len = AlignUp(len, 32);
  *sp = *sp - len;
  arg_ptr = reinterpret_cast<uint64_t *>(*sp);

  // add the argument count
  *arg_ptr++ = argv.size();

  // add arguments to the stack
  for (auto &arg : argv) {
    *arg_ptr++ = reinterpret_cast<uintptr_t>(info_block_ptr);
    std::memcpy(info_block_ptr, arg.data(), arg.size());
    info_block_ptr[arg.size()] = 0;
    info_block_ptr += arg.size() + 1;
  }

  // null terminate the arg array
  *arg_ptr++ = 0;

  // add environment variables to the stack
  for (auto &arg : envp) {
    *arg_ptr++ = reinterpret_cast<uintptr_t>(info_block_ptr);
    std::memcpy(info_block_ptr, arg.data(), arg.size());
    info_block_ptr[arg.size()] = 0;
    info_block_ptr += arg.size() + 1;
  }

  // null terminate the env array
  *arg_ptr++ = 0;

  // add the auxiliary vector to the stack
  SetupAuxVec(
      reinterpret_cast<std::array<Elf64_auxv_t, kNumAuxVectors> *>(arg_ptr),
      filename, edata, random_ptr);
}

}  // namespace

struct ExecContext {
  ExecContext(FSRoot &fs, std::vector<std::string_view> &args)
      : fs(fs), argv_view_(std::move(args)) {}
  ExecContext(FSRoot &fs, std::vector<std::string> &args)
      : fs(fs), argv_(std::move(args)) {}

  FSRoot &fs;
  JunctionFile file;
  elf_header hdr;

  const std::vector<std::string_view> &get_argv_view() {
    if (argv_.size() && !argv_view_.size()) {
      argv_view_.reserve(argv_.size());
      for (auto &arg : argv_) argv_view_.emplace_back(arg);
    }
    return argv_view_;
  }

  // Turns the arguments of a script into those of its interpreter: argv[0]
  // becomes the script's pathname, as it was given to execve(), and the
  // interpreter line goes in front. The pathname is how the interpreter finds
  // the script; argv[0] is only what the caller chose to call it, which for a
  // script found on PATH is a bare name that opens nothing.
  void PrependArgs(std::vector<std::string_view> &tokens,
                   std::string_view script_path) {
    TakeArgvOwnership();
    if (argv_.empty())
      argv_.emplace_back(script_path);
    else
      argv_[0] = std::string(script_path);
    argv_.insert(argv_.begin(), tokens.begin(), tokens.end());
  }

  void TakeArgvOwnership() {
    if (!argv_.size()) {
      argv_.reserve(argv_view_.size());
      for (auto &arg : argv_view_) argv_.emplace_back(arg);
    }
    argv_view_.clear();
  }

 private:
  std::vector<std::string> argv_;
  std::vector<std::string_view> argv_view_;
};

Status<void> ResolveElf(std::shared_ptr<DirectoryEntry> dent, ExecContext &ctx,
                        std::string_view pathname, bool must_be_reloc,
                        size_t max_depth = kMaxInterpFollow) {
  Status<JunctionFile> file =
      JunctionFile::Open(std::move(dent), 0, FileMode::kRead);
  if (!file) return MakeError(file);

  Status<void> ret = CheckELFLoad(*file, ctx.hdr, must_be_reloc);
  if (ret) {
    ctx.file = std::move(*file);
    return {};
  }

  if (max_depth == 0) return MakeError(ELOOP);

  file->Seek(0);
  StreamBufferReader r(*file, 256);
  std::istream instream(&r);
  // Neither an ELF image nor a "#!" script: ENOEXEC, and it matters that it is
  // exactly that. A shell that gets ENOEXEC from execve() runs the file with
  // /bin/sh itself, which is how a script with no interpreter line works at
  // all -- R's bin/INSTALL starts with a comment, not "#!". EINVAL made dash
  // report "exec: .../INSTALL: Invalid argument" and every R package build
  // fail.
  if (instream.get() != '#' || instream.get() != '!') return MakeError(ENOEXEC);

  // As Linux parses it: blanks after "#!" are skipped, the interpreter runs
  // to the next blank, and whatever follows -- trimmed -- is a single optional
  // argument. "#! /bin/sh" is common (Debian's maintainer scripts), and taking
  // the text before its first space as the interpreter gave an empty name.
  std::string s;
  std::getline(instream, s);
  auto is_blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
  std::string_view line(s);
  while (!line.empty() && is_blank(line.front())) line.remove_prefix(1);
  while (!line.empty() && is_blank(line.back())) line.remove_suffix(1);
  if (line.empty()) return MakeError(ENOEXEC);
  std::vector<std::string_view> tokens;
  size_t cut = 0;
  while (cut < line.size() && !is_blank(line[cut])) cut++;
  tokens.push_back(line.substr(0, cut));
  line.remove_prefix(cut);
  while (!line.empty() && is_blank(line.front())) line.remove_prefix(1);
  if (!line.empty()) tokens.push_back(line);

  Status<std::shared_ptr<DirectoryEntry>> path =
      LookupDirEntry(ctx.fs, tokens[0]);
  if (!path) return MakeError(path);

  // tokens view into s, which PrependArgs copies; the interpreter's own path
  // is copied too, since it names the script if that is a script in turn.
  std::string interp(tokens[0]);
  ctx.PrependArgs(tokens, pathname);
  return ResolveElf(std::move(*path), ctx, interp, must_be_reloc,
                    max_depth - 1);
}

Status<ExecInfo> FinishExec(MemoryMap &mm, ExecContext &ctx,
                            const std::vector<std::string_view> &envp) {
  Status<elf_data> edata = DoELFLoad(mm, ctx.file, ctx.fs, ctx.hdr);
  // Record pathname.
  if (!edata) return MakeError(edata);

  mm.set_bin_path(ctx.file.get_dent(), ctx.get_argv_view());

  // Create the first thread
  uint64_t entry =
      edata->interp ? edata->interp->entry_addr : edata->entry_addr;
  // setup a stack
  Status<void *> guard =
      mm.MMapAnonymous(nullptr, kStackSize + kStackSize, PROT_NONE, 0);
  if (!guard) return MakeError(guard);
  void *rsp = reinterpret_cast<void *>(
      (reinterpret_cast<uintptr_t>(*guard) + kStackSize));
  Status<void *> ret = mm.MMapAnonymous(rsp, kStackSize, PROT_READ | PROT_WRITE,
                                        MAP_FIXED | MAP_STACK);
  if (!ret) return MakeError(ret);
  uint64_t sp = reinterpret_cast<uint64_t>(rsp) + kStackSize;

  SetupStack(&sp, ctx.get_argv_view(), envp, *edata);
  return {{sp, entry}};
}

Status<ExecInfo> Exec(Process &p, MemoryMap &mm, std::string_view pathname,
                      std::vector<std::string_view> &argv,
                      const std::vector<std::string_view> &envp,
                      bool must_be_reloc) {
  ExecContext ctx(p.get_fs(), argv);
  Status<std::shared_ptr<DirectoryEntry>> path =
      LookupDirEntry(ctx.fs, pathname);
  if (!path) return MakeError(path);
  Status<void> ret = ResolveElf(std::move(*path), ctx, pathname, must_be_reloc);
  if (!ret) return MakeError(ret);

  if (unlikely(GetCfg().strace_enabled())) {
    Status<std::string> filename = ctx.file.get_dent()->GetPathStr(ctx.fs);
    BUG_ON(!filename);
    LogSyscallDirect("execve", *filename, ctx.get_argv_view(), envp);
  }

  return FinishExec(mm, ctx, envp);
}

// Whether the address space @handle already holds a non-relocatable image,
// other than @except's. With one address space per forked process, that -- not
// how many such images exist in the container -- is what decides whether
// another can be loaded at its fixed address.
bool AddressSpaceHoldsFixedImage(uint64_t handle, const MemoryMap *except) {
  bool found = false;
  Process::ForEachProcess([&](Process &proc) {
    MemoryMap &m = proc.get_mem_map();
    if (&m != except && m.is_non_reloc() && m.get_as_handle() == handle)
      found = true;
  });
  return found;
}

long DoExecve(std::shared_ptr<DirectoryEntry> dent, const char *filename,
              const char *argv[], const char *envp[]) {
  assert(IsOnStack(GetSyscallStack()));

  // We can allocate a thread_tf on the syscall stack but not the
  // FunctionCallTf wrapper. Use the Thread instance's fcall_tf.
  NewThreadTf start_tf;

  Thread &myth = mythread();
  Process &p = myth.get_process();
  MemoryMap &old_mm = p.get_mem_map();

  {
    // allocate new memory map
    Status<std::shared_ptr<MemoryMap>> mm =
        MemoryMap::Create(kMemoryMappingSize);
    if (!mm) return MakeCError(mm);

    // turn argv and envp in string_view vectors, memory must remain valid until
    // after Exec returns
    std::vector<std::string_view> argv_s;
    const char **ptr = argv;
    while (*ptr) argv_s.emplace_back(*ptr++);

    // If exec was called from a process with a non-relocatable executable (that
    // is not in vfork) then we can safely replace it with another
    // non-relocatable executable.
    size_t nr_non_reloc = MemoryMap::get_nr_non_reloc();
    if (nr_non_reloc && old_mm.is_non_reloc() && !p.in_vfork_preexec())
      nr_non_reloc--;

    // With per-process address spaces the container-wide count says nothing:
    // what matters is the address space the image will be loaded into, and
    // that is settled below, once the image's type is known.
    const bool mas = MultiAddressSpaceEnabled();

    ExecContext ctx(p.get_fs(), argv_s);
    Status<void> ret =
        ResolveElf(std::move(dent), ctx, filename, !mas && nr_non_reloc > 0);
    if (!ret) return MakeCError(ret);

    std::vector<std::string_view> envp_view;
    std::vector<std::string> envp_s;

    // We are replacing a non-reloc MM with another non-reloc MM.
    // We need to free existing memory first, since it may overlap.
    // Replacing in place frees the old image first, which is only ours to
    // free if we are not borrowing it: a vfork child's map is its parent's.
    // (The container-wide count used to keep a vfork child from getting here.)
    bool replace_non_reloc = old_mm.is_non_reloc() &&
                             ctx.hdr.type != kETypeDynamic &&
                             !p.in_vfork_preexec();
    // A non-relocatable image needs its fixed addresses free. They are, unless
    // this address space already holds such an image that is not ours to
    // replace: a vfork child's parent's (python spawning python through
    // posix_spawn), or another process exec'd into this same space. Then the
    // process moves into an address space of its own first -- a clone with
    // every guest's memory left out, since exec keeps none of it.
    bool promoted = false;
    // Claimed, not looked up: the claim is atomic, and it is held by whichever
    // map has such an image until that image is really gone (see
    // MemoryMap::ClaimFixedImageSlot).
    if (mas && ctx.hdr.type != kETypeDynamic && !replace_non_reloc &&
        !(*mm)->ClaimFixedImageSlot()) {
      // Everything exec still needs from the old memory, copied out of it.
      ctx.TakeArgvOwnership();
      ptr = envp;
      while (*ptr) envp_s.emplace_back(*ptr++);
      envp_view.reserve(envp_s.size());
      envp_view.insert(envp_view.end(), envp_s.begin(), envp_s.end());
      if (unlikely(GetCfg().strace_enabled()))
        LogSyscallDirect("execve", (strace::PathName *)filename, argv, envp);

      Status<uint64_t> as;
      {
        rt::MutexGuard fg(AddressSpaceForkLock());
        // The same exclusions a fork of the caller would use: its own region
        // stays in (and is dropped from the new space below). Marking a region
        // MADV_DONTFORK while its owner lives in this address space is not
        // something fork ever does, and the owner did not survive it.
        std::vector<AddressRange> exclude =
            MemoryMap::GetOtherGuestRegions(old_mm.get_reservation());
        as = CloneCurrentAddressSpace(exclude);
      }
      if (!as) return MakeCError(as);

      // The new map's region was reserved in the space we are leaving, before
      // it was known that we would leave. The clone has its own copy of that
      // reservation; this one would outlive the process -- a space that dies
      // with its last map unmaps nothing -- and collide with whoever is given
      // the region next.
      {
        AddressRange resv = (*mm)->get_reservation();
        Status<void> um = KernelMUnmap(reinterpret_cast<void *>(resv.start),
                                       resv.Length());
        if (!um) LOG(WARN) << "exec: stale reservation left behind " << um.error();
      }

      // From here the old memory is gone for this thread: %fs must stop
      // pointing into it, and the scheduler must bind this process's threads
      // to the new space even if the load below blocks.
      SetFSBase(perthread_read(runtime_fsbase));
      myth.get_rseq().reset();
      (*mm)->OwnAddressSpace(*as);
      // A new address space: its slot is free, and now ours.
      if (unlikely(!(*mm)->ClaimFixedImageSlot()))
        LOG(ERR) << "exec: fixed-image slot of a new address space is taken";
      // Keep the old map alive until we are done with it below: for a forked
      // process this is its last reference, and @old_mm refers to it.
      std::shared_ptr<MemoryMap> old_keep = p.AdoptMemoryMap(*mm);
      // With preemption off: the binding and the per-core record of it must
      // change together. A migration between the two leaves a core bound to
      // the new space while recording the old one, and the next thread that
      // wants the old one -- the vfork parent -- skips the switch and runs
      // where its memory does not exist.
      rt::Preempt::Lock();
      ActivateAddressSpace(*as);
      rt::Preempt::Unlock();
      // The clone carried a copy of the caller's memory, fixed image
      // included. None of it survives an exec; drop it from this space.
      old_mm.UnmapInheritedCopyHere();
      // And whatever of other guests' memory the clone picked up (see
      // SweepClonedAddressSpace). Ours, in it, is the new map's reservation.
      SweepClonedAddressSpace(
          *as, MemoryMap::GetOtherGuestRegions((*mm)->get_reservation()));
      // Now the old map may go. Its destructor visits or releases the space
      // we left and comes back to this one.
      old_keep.reset();
      promoted = true;
    } else if (replace_non_reloc) {
      // Log while the memory is still available.
      if (unlikely(GetCfg().strace_enabled()))
        LogSyscallDirect("execve", (strace::PathName *)filename, argv, envp);

      ctx.TakeArgvOwnership();

      ptr = envp;
      while (*ptr) envp_s.emplace_back(*ptr++);
      envp_view.reserve(envp_s.size());
      envp_view.insert(envp_view.end(), envp_s.begin(), envp_s.end());

      // Stop any other threads that may be using memory from the old MM.
      p.KillThreadsAndWait();
      // Reset rseq since its memory may become invalid (and may be accessed if
      // preemption occurs).
      myth.get_rseq().reset();
      old_mm.UnmapAll();
      // Same address space, same addresses: the slot passes to the new map.
      MemoryMap::TransferFixedImageSlot(old_mm, **mm);
    } else {
      ptr = envp;
      while (*ptr) envp_view.emplace_back(*ptr++);
    }

    Status<ExecInfo> regs = FinishExec(**mm, ctx, envp_view);
    if (!regs) {
      if (replace_non_reloc || promoted) {
        LOG(ERR) << "failed to replace non-relocatable image";
        syscall_exit(-1);
      }
      return MakeCError(regs);
    }

    // The syscall has suceeded.
    if (unlikely(GetCfg().strace_enabled() && !replace_non_reloc && !promoted))
      LogSyscallDirect((long)0, std::string_view("execve"),
                       (strace::PathName *)filename, argv, envp);

    // Reset rseq since its memory may become invalid (and may be accessed if
    // preemption occurs).
    myth.get_rseq().reset();

    // exec replaces a process's mappings but leaves it in the same address
    // space: the new image was just loaded into whichever address space this
    // core is bound to, which is the caller's -- its own if it forked, its
    // parent's if it vforked. Without this the new MemoryMap keeps the default
    // handle (kRootAddressSpace), so the next time the thread is scheduled the
    // core binds the *root* address space and jmp_thread_direct restores its
    // context from memory that only exists in the caller's -- a fault at the
    // moment of the switch, with no frame to report it from. (`sleep 1 &` is
    // enough to show it; see docs/bug-exec-timer-address-space.md.)
    //
    // The new map becomes one more holder of that space, not its owner: a
    // vfork child's parent is still living there.
    if (!promoted) (*mm)->ShareAddressSpaceOf(old_mm);

    // Dropping the old map unmaps the old image, and %fs still points into it:
    // a guest syscall runs on the guest's TCB, so any LibOS code that touches
    // thread-local storage between here and the jump into the new image would
    // fault on freed memory. Put the LibOS fsbase back first; the new program's
    // is installed when it starts.
    SetFSBase(perthread_read(runtime_fsbase));

    // Complete the exec
    p.FinishExec(std::move(*mm));

    // clear argument registers
    start_tf.rdi = 0;
    start_tf.rsi = 0;
    start_tf.rdx = 0;
    start_tf.rcx = 0;
    start_tf.r8 = 0;
    start_tf.r9 = 0;

    start_tf.rsp = std::get<0>(*regs);
    start_tf.rip = std::get<1>(*regs);
  }

  myth.OnExec();

  // Set entry_regs to start_tf and use fcall_tf to unwind.
  myth.ReplaceEntryRegs(start_tf).JmpUnwindSysret(myth);
  std::unreachable();
}

long usys_execve(const char *filename, const char *argv[], const char *envp[]) {
  // Exec may destroy the current stack, switch before proceeding.
  return CallOnSyscallStack([&] {
    Status<std::shared_ptr<DirectoryEntry>> dent =
        LookupDirEntry(myproc().get_fs(), filename);
    if (!dent) return static_cast<long>(MakeCError(dent));
    return DoExecve(std::move(*dent), filename, argv, envp);
  });
}

long usys_execveat(int fd, const char *filename, const char *argv[],
                   const char *envp[], int flags) {
  // Exec may destroy the current stack, switch before proceeding.
  return CallOnSyscallStack([&] {
    Status<std::shared_ptr<DirectoryEntry>> dent =
        LookupDirEntry(myproc(), fd, filename);
    if (!dent) return static_cast<long>(MakeCError(dent));
    return DoExecve(std::move(*dent), filename, argv, envp);
  });
}

}  // namespace junction
