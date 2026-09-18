#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "junction/base/compiler.h"
#include "junction/base/error.h"
#include "junction/bindings/net.h"
#include "junction/bindings/runtime.h"

#ifndef CUSTOM_GLIBC_INTERPRETER_PATH
#define CUSTOM_GLIBC_INTERPRETER_PATH
#endif

#ifndef CUSTOM_GLIBC_DIR
#define CUSTOM_GLIBC_DIR
#endif

#ifndef CUSTOM_GLIBC_PRELOAD
#define CUSTOM_GLIBC_PRELOAD
#endif

namespace junction {

class Process;

class alignas(kCacheLineSize) JunctionCfg {
 public:
  [[nodiscard]] std::string_view get_chroot_path() const { return chroot_path; }

  [[nodiscard]] std::string_view get_fs_config_path() const {
    return fs_config_path;
  }

  [[nodiscard]] std::string_view get_interp_path() const { return interp_path; }

  [[nodiscard]] std::string_view get_ld_path() const { return ld_path; }

  [[nodiscard]] const std::string &get_glibc_path() const { return glibc_path; }

  [[nodiscard]] const std::string &get_preload_path() const {
    return preload_path;
  }

  [[nodiscard]] const std::vector<std::string> &get_binary_envp() const {
    return binary_envp;
  }

  [[nodiscard]] uid_t get_gid() const { return gid_; }
  [[nodiscard]] uid_t get_uid() const { return uid_; }

  [[nodiscard]] bool strace_enabled() const { return strace; }
  [[nodiscard]] bool restoring() const { return restore; }
  [[nodiscard]] bool kernel_restoring() const { return kernel_restoring_; }
  [[nodiscard]] bool expecting_snapshot() const { return expecting_snapshot_; }
  [[nodiscard]] bool jif() const { return jif_; }
  [[nodiscard]] bool stack_switch_enabled() const { return stack_switching; }
  [[nodiscard]] bool madv_dontneed_remap() const { return madv_remap; }
  [[nodiscard]] bool cache_linux_fs() const { return cache_linux_fs_; }
  [[nodiscard]] bool restore_populate() const { return restore_populate_; }
  [[nodiscard]] int snapshot_on_stop() const { return snapshot_on_stop_; }
  [[nodiscard]] bool mem_trace() const { return mem_trace_; }
  [[nodiscard]] bool snapshot_terminate() const {
    return terminate_after_snapshot_;
  }
  [[nodiscard]] uint16_t port() const { return port_; }

  [[nodiscard]] bool using_chroot() const { return chroot_path.size() > 0; }
  [[nodiscard]] bool zpoline() const { return zpoline_; }

  // Whether guest processes may be given address spaces of their own. When
  // false, Junction behaves as it did before multi-address-space support:
  // every guest shares one address space and fork() reports ENOSYS.
  [[nodiscard]] bool mas_enabled() const { return mas_enabled_; }

  // Megabytes the LibOS should deliberately allocate through its own glibc on
  // the first guest getpid(), to observe where such a mapping lands. Zero
  // disables the probe. Diagnostic only.
  [[nodiscard]] size_t debug_libos_alloc_mb() const {
    return debug_libos_alloc_mb_;
  }
  [[nodiscard]] bool debug_libos_escape() const { return debug_libos_escape_; }

  // Whether to diff the LibOS's mappings across every live address space after
  // each fork. Diagnostic only; see AuditAddressSpaceCoherence().
  [[nodiscard]] bool debug_as_audit() const { return debug_as_audit_; }

  // Whether to deliberately violate the frozen-after-init invariant once, to
  // test the check that reports it. See FrozenViolationProbe().
  [[nodiscard]] bool debug_frozen_probe() const { return debug_frozen_probe_; }

  // Whether to run the LibOS arena's self-test after the first fork. See
  // ArenaProbe().
  [[nodiscard]] bool debug_arena_probe() const { return debug_arena_probe_; }

  // Seconds of no forward progress before the hang watchdog dumps every
  // thread. Zero disables it.
  [[nodiscard]] size_t debug_hang_watchdog_s() const {
    return debug_hang_watchdog_s_;
  }

  // Path for the LibOS memory trace, empty when disabled.
  [[nodiscard]] const std::string &get_memtrace_path() const {
    return memtrace_path_;
  }

  static void PrintOptions();
  Status<void> FillFromArgs(int argc, char *argv[]);
  void Print();

  static JunctionCfg &get() { return singleton_; };
  static std::string GetArg(const std::string &arg);
  static bool GetBool(const std::string &name);

 private:
  // Hot state
  bool strace;
  bool madv_remap;
  bool expecting_snapshot_;
  bool restore_populate_;
  bool zpoline_;
  bool mas_enabled_;
  std::string memtrace_path_;
  size_t debug_libos_alloc_mb_{0};
  bool debug_libos_escape_{false};
  bool debug_as_audit_{false};
  bool debug_frozen_probe_{false};
  bool debug_arena_probe_{false};
  size_t debug_hang_watchdog_s_{0};
  uid_t gid_;
  uid_t uid_;
  std::string chroot_path;

  // Cold state
  std::string fs_config_path;
  std::string interp_path;
  std::string glibc_path;
  std::string ld_path;
  std::string preload_path;
  std::vector<std::string> binary_envp;

  uint16_t port_;
  bool restore;
  bool kernel_restoring_;
  bool jif_;
  bool stack_switching;
  bool cache_linux_fs_;
  bool terminate_after_snapshot_;
  int snapshot_on_stop_;
  bool mem_trace_;
  static JunctionCfg singleton_;
};

inline JunctionCfg &GetCfg() { return JunctionCfg::get(); }

std::string_view GetLinuxCwd();
pid_t GetLinuxPid();

Status<void> init();
Status<void> InitUnixTime();
Status<void> InitControlServer();
Status<void> InitChannelClient();
Status<void> InitZpoline();
Status<std::unique_ptr<Process>> InitTestProc();
void MarkRuntimeReady();
[[nodiscard]] bool IsRuntimeReady();

// statically cast an instance of type T to type U in release mode, dynamically
// cast in debug mode.
template <typename U, typename T>
U fast_cast(T &&t) {
  if constexpr (is_debug_build()) {
    rt::RuntimeLibcGuard g;
    return dynamic_cast<U>(std::forward<T>(t));
  }
  return static_cast<U>(std::forward<T>(t));
}

template <typename U, typename T>
U dynamic_cast_guarded(T &&t) {
  rt::RuntimeLibcGuard g;
  return dynamic_cast<U>(std::forward<T>(t));
}

template <typename U, typename T>
const U fast_cast(const T &t) {
  if constexpr (is_debug_build()) {
    rt::RuntimeLibcGuard g;
    return dynamic_cast<const U>(t);
  }
  return static_cast<const U>(t);
}

template <typename U, typename T>
std::shared_ptr<U> fast_pointer_cast(std::shared_ptr<T> t) {
  if constexpr (is_debug_build()) {
    rt::RuntimeLibcGuard g;
    return std::dynamic_pointer_cast<U>(std::move(t));
  }
  return std::static_pointer_cast<U>(std::move(t));
}

}  // namespace junction
