
#include <boost/program_options.hpp>
// Include base/assert.h now to ensure correct definition of assert is used.
extern "C" {
#include <base/assert.h>
}

#include <fstream>
#include <iostream>
#include <memory>

#include "junction/base/error.h"
#include "junction/bindings/log.h"
#include "junction/fs/fs.h"
#include "junction/junction.h"
extern "C" {
#include <base/mem.h>
}

#include "junction/kernel/arena.h"
#include "junction/kernel/as.h"
#include "junction/kernel/memtrace.h"
#include "kern/junction_as.h"
#include "junction/bindings/timer.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/signal.h"
#include "junction/shim/backend/init.h"
#include "junction/syscall/seccomp.h"
#include "junction/syscall/syscall.h"

namespace junction {

// Pairs of (mount point, host path) for additional linux filesystems to be
// mounted.
const std::vector<std::pair<std::string, std::string>> linux_mount_points = {
    {"/tmp", "/tmp"},
    {"/home", "/home"},
    {"/dev/shm", "/dev/shm"},
    {"/fast", "/fast"},
};

pid_t linux_pid;

pid_t GetLinuxPid() { return linux_pid; }

JunctionCfg JunctionCfg::singleton_;

extern "C" void log_message_begin(uint64_t *cb_data) {
  if (base_init_done && thread_self() != NULL) {
    preempt_disable();
    *cb_data = GetFSBase();
    SetFSBase(perthread_read(runtime_fsbase));
  }
}

extern "C" void log_message_end(uint64_t *cb_data) {
  if (base_init_done && thread_self() != NULL) {
    SetFSBase(*cb_data);
    preempt_enable();
  }
}

namespace po = boost::program_options;

po::options_description GetOptions() {
  po::options_description desc("Junction options");
  desc.add_options()                      //
      ("help,h", "produce help message")  //
      ("chroot_path", po::value<std::string>()->default_value("/"),
       "chroot path to execute the binary from")  //
      ("fs_config_path", po::value<std::string>()->default_value(""),
       "file system configuration path")  //
      ("interpreter_path",
       po::value<std::string>()->implicit_value("")->default_value(
           CUSTOM_GLIBC_INTERPRETER_PATH),
       "use this custom interpreter for binaries")  //
      ("glibc_path",
       po::value<std::string>()->implicit_value("")->default_value(
           CUSTOM_GLIBC_DIR),
       "path to custom libc")  //
      ("ld_path",
       po::value<std::string>()->implicit_value("")->default_value(""),
       "a path to include in LD_LIBRARY_PATH")  //
      ("ld_preload",
       po::value<std::string>()->implicit_value("")->default_value(""),
       "location of ld preload library")  //
      ("env,E", po::value<std::vector<std::string>>()->multitoken(),
       "environment flags for binary")  //
      ("uid,u", po::value<uid_t>()->default_value(0),
       "UID shown to processes internally")  //
      ("gid,g", po::value<uid_t>()->default_value(0),
       "GID shown to processes internally")  //
      ("port,p", po::value<int>()->default_value(42),
       "port number to setup control port on")                              //
      ("strace,s", po::bool_switch()->default_value(false), "strace mode")  //
      ("restore,r", po::bool_switch()->default_value(false),
       "restore from a snapshot")  //
      ("kernel-restore,k", po::bool_switch()->default_value(false),
       "restore JIFs through kernel module")                         //
      ("jif", po::bool_switch()->default_value(false), "use a jif")  //
      ("loglevel,l", po::value<int>()->default_value(LOG_DEBUG),
       "the maximum log level to print")  //
      ("mem-trace", po::bool_switch()->default_value(false),
       "trace the memory addresses")  //
      ("mem-trace-out",
       po::value<std::string>()->implicit_value("")->default_value(""),
       "path to store the memory address trace")  //
      ("snapshot-on-stop,S",
       po::value<int>()->default_value(0)->implicit_value(1),
       "take a snapshot when the main process stops (after N stops)")  //
      ("snapshot-prefix", po::value<std::string>()->default_value(""),
       "snapshot prefix path (will generate <prefix>.metadata and "
       "<prefix>.elf")  //
      ("stackswitch", po::bool_switch()->default_value(false),
       "use stack switching syscalls")  //
      ("debug_libos_escape", po::bool_switch(),
       "probe whether a LibOS allocation can land outside an arena slot")
      ("debug_libos_alloc", po::value<size_t>()->default_value(0),
       "MB for the LibOS to allocate via its own glibc on the first guest "
       "getpid(), to observe where the mapping lands (diagnostic)")  //
      ("trace_libos_mem", po::value<std::string>()->default_value(""),
       "trace every memory mapping operation to this file")  //
      ("debug_hang_watchdog", po::value<size_t>()->default_value(0),
       "dump every process and thread if no guest thread accrues runtime for "
       "this many seconds (0 disables). For diagnosing hangs where every "
       "kthread parks and nothing is runnable.")  //
      ("debug_arena_probe", po::bool_switch(),
       "after the first fork, map LibOS arena memory here and read it back "
       "from every other address space, to test the shadow-map repair (test "
       "only)")  //
      ("debug_frozen_probe", po::bool_switch(),
       "after the first fork, deliberately change LibOS memory in one address "
       "space, to test that the frozen-invariant check notices (test only)")  //
      ("debug_as_audit", po::bool_switch(),
       "after every fork, check that all live address spaces agree on the "
       "LibOS's mappings (diagnostic; costs a /proc read per address space)")  //
      ("no_mas", po::bool_switch()->default_value(false),
       "disable per-guest address spaces; fork() will report ENOSYS and the "
       "LibOS keeps its memory private")  //
      ("zpoline", po::bool_switch()->default_value(false),
       "hotpatch syscall instructions using the zpoline technique")  //
      ("madv_remap", po::bool_switch()->default_value(false),
       "zero memory when MADV_FREE is used (intended for profiling)")  //
      ("cache_linux_fs", po::bool_switch()->default_value(false),
       "cache directory structure of the linux filesystem")  //
      ("snapshot_enabled", po::bool_switch()->default_value(false),
       "this runtime may be snapshotted")  //
      ("restore_populate", po::bool_switch()->default_value(false),
       "use MAP_POPULATE on restore")  //
      ("snapshot_terminate", po::bool_switch()->default_value(false),
       "terminate after snapshot")  //
      ("function_arg", po::value<std::string>()->default_value(""),
       "argument provided to serverless function")  //
      ("function_name", po::value<std::string>()->default_value("func"),
       "name of function being run")                               //
      ("keep_alive", po::bool_switch()->default_value(false), "")  //
      ("cwd", po::value<std::string>()->default_value(""),
       "current working directory at start");  //
  return desc;
}

void JunctionCfg::PrintOptions() { std::cerr << GetOptions(); }

po::variables_map vm;

std::string JunctionCfg::GetArg(const std::string &name) {
  return vm[name].as<std::string>();
}

bool JunctionCfg::GetBool(const std::string &name) {
  return vm[name].as<bool>();
}

Status<void> JunctionCfg::FillFromArgs(int argc, char *argv[]) {
  po::options_description desc = GetOptions();

  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (std::exception &e) {
    std::cerr << "parse error: " << e.what() << std::endl;
    return MakeError(EINVAL);
  }

  if (vm.count("help")) return MakeError(EINVAL);

  chroot_path = vm["chroot_path"].as<std::string>();
  fs_config_path = vm["fs_config_path"].as<std::string>();
  interp_path = vm["interpreter_path"].as<std::string>();
  glibc_path = vm["glibc_path"].as<std::string>();
  ld_path = vm["ld_path"].as<std::string>();
  preload_path = vm["ld_preload"].as<std::string>();

  if (vm.count("env")) binary_envp = vm["env"].as<std::vector<std::string>>();

  restore = vm["restore"].as<bool>();

  snapshot_on_stop_ = vm["snapshot-on-stop"].as<int>();
  expecting_snapshot_ = vm["snapshot_enabled"].as<bool>();
  expecting_snapshot_ |= snapshot_on_stop_;
  expecting_snapshot_ |=
      vm["function_arg"].as<std::string>().size() > 0 && !restore;

  uid_ = vm["uid"].as<uid_t>();
  gid_ = vm["gid"].as<uid_t>();
  zpoline_ = vm["zpoline"].as<bool>();
  if (zpoline_ && !CPUHasPKUSupport()) {
    zpoline_ = false;
    std::cerr << "zpoline disabled: no support for execute-only memory"
              << std::endl;
  }

  strace = vm["strace"].as<bool>();
  stack_switching = vm["stackswitch"].as<bool>();
  max_loglevel = vm["loglevel"].as<int>();
  madv_remap = vm["madv_remap"].as<bool>();
  kernel_restoring_ = vm["kernel-restore"].as<bool>();
  jif_ = vm["jif"].as<bool>();
  cache_linux_fs_ = vm["cache_linux_fs"].as<bool>();
  port_ = vm["port"].as<int>();
  if (snapshot_on_stop_ && vm["snapshot-prefix"].as<std::string>().empty()) {
    std::cerr << "need a snapshot prefix if we are snapshotting" << std::endl;
    return MakeError(EINVAL);
  }

  restore_populate_ = vm["restore_populate"].as<bool>();
  mem_trace_ = vm["mem-trace"].as<bool>();
  terminate_after_snapshot_ = vm["snapshot_terminate"].as<bool>();

  // Per-guest address spaces are available when the device is present, unless
  // they have been turned off. This is decided here, before the Caladan
  // runtime starts, because the runtime has to know whether to keep the
  // LibOS's memory shared -- which is only worth doing if guests will ever be
  // cloned into separate address spaces.
  memtrace_path_ = vm["trace_libos_mem"].as<std::string>();
  debug_libos_alloc_mb_ = vm["debug_libos_alloc"].as<size_t>();
  debug_libos_escape_ = vm["debug_libos_escape"].as<bool>();
  debug_as_audit_ = vm["debug_as_audit"].as<bool>();
  debug_frozen_probe_ = vm["debug_frozen_probe"].as<bool>();
  debug_arena_probe_ = vm["debug_arena_probe"].as<bool>();
  debug_hang_watchdog_s_ = vm["debug_hang_watchdog"].as<size_t>();
  mas_enabled_ = !vm["no_mas"].as<bool>() &&
                 access(JUNCTION_AS_DEVICE, R_OK | W_OK) == 0;
  cfg_shared_runtime_mem = mas_enabled_;

  // Guest processes that get their own address spaces need Junction's system
  // call handlers to run on Junction's own stacks. Anything the LibOS leaves on
  // a guest stack -- a Caladan timer entry for nanosleep(), say -- lives in
  // that guest's memory, and is unreachable from any other address space.
  if (mas_enabled_) stack_switching = true;

  if (mem_trace_ && !stack_switching) {
    std::cerr << "Enabling stack switching for memory tracing" << std::endl;
    stack_switching = true;
  }

  return {};
}

void JunctionCfg::Print() {
  LOG(INFO) << "cfg: chroot_path = " << chroot_path;
  LOG(INFO) << "cfg: fs_config_path = " << fs_config_path;
  LOG(INFO) << "cfg: interpreter_path = " << interp_path;
  LOG(INFO) << "cfg: glibc_path = " << glibc_path;
  LOG(INFO) << "cfg: ld_path = " << ld_path;
  LOG(INFO) << "cfg: ld_preload = " << preload_path;
  for (std::string &s : binary_envp) LOG(INFO) << "env: " << s;
}

Status<void> InitChroot() {
  std::string_view chroot_path = GetCfg().get_chroot_path();
  if (chroot_path != "/") {
    if (chroot(chroot_path.data())) return MakeError(errno);
    if (chdir("/")) return MakeError(errno);
  }
  return {};
}

std::vector<std::string> GetFsMounts() {
  std::string_view path = GetCfg().get_fs_config_path();
  if (path.empty()) return {};
  std::vector<std::string> paths;
  std::ifstream f(path.data());
  std::string line;
  while (std::getline(f, line)) paths.emplace_back(line);
  return paths;
}

Status<void> init() {
  // Make sure any one-time routines in the logger get run now.
  LOG(INFO) << "Initializing junction";
  GetCfg().Print();

  linux_pid = getpid();

  Status<void> ret = InitSignal();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize signal: " << ret.error();
    return ret;
  }

  ret = SyscallInit();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize syscall: " << ret.error();
    return ret;
  }

  // Start tracing before anything else maps memory, so the trace is complete.
  ret = InitMemTrace(GetCfg().get_memtrace_path());
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to open the memory trace: " << ret.error();
    return ret;
  }

  // Must run before any guest exists: it rewrites the LibOS's own private
  // mappings as shared ones, and it can only tell LibOS memory from guest
  // memory while there is no guest memory.
  ret = InitAddressSpaces();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize address spaces: " << ret.error();
    return ret;
  }

  // The LibOS arena: where LibOS memory goes so that any address space can
  // repair an absence from the memfd behind it. Before any guest exists, for
  // the same reason as above -- its reservation and its node pool have to be
  // in every address space, and only a clone puts them there.
  ret = InitLibOSArena();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize the LibOS arena: " << ret.error();
    return ret;
  }
  StartArenaGc();

  // Hang watchdog. Runs as an ordinary runtime thread, which keeps being
  // scheduled even when every guest thread is blocked -- that is exactly the
  // state it exists to catch, and it is the one state where nothing else will
  // ever report anything.
  if (size_t secs = GetCfg().debug_hang_watchdog_s(); secs > 0) {
    rt::Spawn([secs] {
      Duration last(0);
      size_t stuck = 0;
      bool dumped = false;
      while (true) {
        rt::Sleep(Duration(1000000));  // 1 s
        Duration total(0);
        Process::ForEachProcess([&](Process &p) { total += p.GetRuntime(); });
        // Guest runtime is the progress metric: it advances whenever any guest
        // thread runs, and costs nothing to sample because the scheduler is
        // already accounting it.
        if (total == last) {
          if (++stuck >= secs && !dumped) {
            dumped = true;
            Process::DumpAllThreads("hang watchdog: no guest runtime accrued");
          }
        } else {
          stuck = 0;
          dumped = false;
        }
        last = total;
      }
    });
    LOG(INFO) << "watchdog: dumping all threads after " << secs
              << "s without progress";
  }

  ret = InitZpoline();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize zpoline: " << ret.error();
    return ret;
  }

  ret = InitChroot();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize chroot: " << ret.error();
    return ret;
  }

  char *dropuid = getenv("DROP_PRIV_UID");
  if (dropuid) {
    int uid = atoi(dropuid);
    if (setgid(uid) != 0 || setuid(uid) != 0) {
      LOG(ERR) << "failed to drop priveleges " << Error(errno);
      return MakeError(errno);
    }
  }

  ret = InitFs(linux_mount_points, GetFsMounts());
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize fs: " << ret.error();
    return ret;
  }

  ret = ShimJmpInit();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize shim: " << ret.error();
    return ret;
  }

  ret = InitUnixTime();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize unix time: " << ret.error();
    return ret;
  }

  ret = init_seccomp();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize seccomp: " << ret.error();
    return ret;
  }

  ret = InitControlServer();
  if (unlikely(!ret)) {
    LOG(ERR) << "failed to initialize control server: " << ret.error();
    return ret;
  }

  return InitChannelClient();
}

}  // namespace junction
