// futex.h - support for futex synchronization

#pragma once

#include <climits>
#include <functional>
#include <optional>

#include "junction/base/arch.h"
#include "junction/base/error.h"
#include "junction/base/intrusive.h"
#include "junction/bindings/sync.h"
#include "junction/bindings/timer.h"
#include "junction/kernel/proc.h"

namespace junction {

namespace detail {

struct futex_waiter {
  futex_waiter(rt::ThreadWaker *waker, uint32_t *key, uint32_t bitset,
               const void *mm, bool shared)
      : waker(waker), key(key), bitset(bitset), mm(mm), shared(shared) {}

  // An address names memory only together with the address space it is in.
  // Forked processes have identical layouts, so two of them wait on the same
  // address all the time; with the address alone as the key, one process's
  // FUTEX_WAKE(addr, 1) was delivered to the other's waiter -- a spurious
  // wakeup there, and a lost one here, for good. A wake matches a waiter of
  // the same memory map, or, as on Linux, across processes only when both the
  // wait and the wake are process-shared (no FUTEX_PRIVATE_FLAG).
  [[nodiscard]] bool Matches(const uint32_t *k, uint32_t bits, const void *wmm,
                             bool wshared) const {
    if (key != k || !(bitset & bits)) return false;
    return mm == wmm || (shared && wshared);
  }

  rt::ThreadWaker *waker;
  uint32_t *key;
  uint32_t bitset;
  const void *mm;
  bool shared;
  IntrusiveListNode node;
};

struct alignas(kCacheLineSize) futex_bucket {
  IntrusiveList<futex_waiter, &futex_waiter::node> futexes;
  rt::Spin lock;
};

}  // namespace detail

inline constexpr uint32_t kFutexBitsetAny = 0xFFFFFFFF;

class alignas(kCacheLineSize) FutexTable {
 public:
  FutexTable() = default;
  ~FutexTable() = default;

  FutexTable(FutexTable &&) = delete;
  FutexTable &operator=(FutexTable &&) = delete;
  FutexTable(const FutexTable &) = delete;
  FutexTable &operator=(const FutexTable &) = delete;

  // Wait blocks on the address @key. However, it returns ETIMEDOUT if the
  // timeout expires, or EAGAIN if @val doesn't match the value in the address.
  // @mm identifies the caller's address space (its MemoryMap); @shared says
  // the operation was not FUTEX_PRIVATE.
  Status<void> Wait(const void *mm, bool shared, uint32_t *key, uint32_t val,
                    uint32_t bitset = kFutexBitsetAny,
                    std::optional<Time> timeout = {});

  // Wake unblocks up to @n threads waiting on the address @key. Returns the
  // number of threads woken.
  int Wake(const void *mm, bool shared, uint32_t *key, int n = INT_MAX,
           uint32_t bitset = kFutexBitsetAny);

  static FutexTable &GetFutexTable();

 private:
  static constexpr size_t kBuckets = 16;  // TODO(amb): allocate dynamically?

  // gets the right hash bucket for a key.
  detail::futex_bucket &get_bucket(uint32_t *key) {
    return buckets_[std::hash<uint32_t *>{}(key) % kBuckets];
  }

  detail::futex_bucket buckets_[kBuckets];

 public:
  // Prints every blocked waiter: the key it is waiting on and the value
  // currently at that key. A waiter whose key still holds the value it is
  // waiting to change is the shape of a wake that can never succeed -- as
  // opposed to a wake that never happened. Diagnostics only; see
  // Process::DumpAllThreads().
  template <typename Fn>
  void ForEachWaiter(Fn fn) {
    for (size_t i = 0; i < kBuckets; i++) {
      rt::SpinGuard g(buckets_[i].lock);
      for (auto &w : buckets_[i].futexes) fn(w.key, w.bitset);
    }
  }
};

}  // namespace junction
