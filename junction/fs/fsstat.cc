#include "junction/fs/fsstat.h"

#include <atomic>

#include "junction/base/time.h"
#include "junction/bindings/log.h"
#include "junction/bindings/thread.h"
#include "junction/bindings/timer.h"
#include "junction/fs/file.h"
#include "junction/fs/fs.h"

namespace junction {

namespace {

constexpr uint64_t kFsStatPeriodUs = 10'000'000;  // 10 s

struct alignas(64) KindCounters {
  std::atomic<uint64_t> opens{0};        // open() calls
  std::atomic<uint64_t> distinct{0};     // inodes opened at least once
  std::atomic<uint64_t> reopened{0};     // inodes opened at least twice
  std::atomic<uint64_t> read_bytes{0};
  std::atomic<uint64_t> write_bytes{0};
  std::atomic<uint64_t> reads{0};
  std::atomic<uint64_t> writes{0};
};

KindCounters kinds[kFsKinds];
std::atomic<int64_t> memfs_files{0};
std::atomic<int64_t> memfs_bytes{0};
std::atomic<int64_t> memfs_files_peak{0};
std::atomic<int64_t> memfs_bytes_peak{0};
std::atomic<uint64_t> dir_opens{0};
const char *const kind_names[kFsKinds] = {"memfs", "linuxfs", "other"};
Time start_time;

void RaisePeak(std::atomic<int64_t> &peak, int64_t v) {
  int64_t cur = peak.load(std::memory_order_relaxed);
  while (v > cur &&
         !peak.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

}  // namespace

void FsStatNoteOpen(Inode &ino) {
  if (ino.is_dir()) {
    dir_opens.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  KindCounters &k = kinds[ino.fs_kind()];
  k.opens.fetch_add(1, std::memory_order_relaxed);
  uint32_t n = ino.note_open();
  if (n == 1) k.distinct.fetch_add(1, std::memory_order_relaxed);
  if (n == 2) k.reopened.fetch_add(1, std::memory_order_relaxed);
}

void FsStatNoteIo(const File *f, size_t bytes, bool is_write) {
  const Inode *ino = f ? f->get_inode() : nullptr;
  KindCounters &k = kinds[ino ? ino->fs_kind() : kFsOther];
  if (is_write) {
    k.writes.fetch_add(1, std::memory_order_relaxed);
    k.write_bytes.fetch_add(bytes, std::memory_order_relaxed);
  } else {
    k.reads.fetch_add(1, std::memory_order_relaxed);
    k.read_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

void FsStatMemfsFiles(int delta) {
  int64_t v = memfs_files.fetch_add(delta, std::memory_order_relaxed) + delta;
  RaisePeak(memfs_files_peak, v);
}

void FsStatMemfsBytes(int64_t delta) {
  int64_t v = memfs_bytes.fetch_add(delta, std::memory_order_relaxed) + delta;
  RaisePeak(memfs_bytes_peak, v);
}

// One line, key=value, so a regex gets it out of the log:
//   fsstat: why=periodic t=123.4 memfs_files=N memfs_bytes=N memfs_files_peak=N
//   memfs_bytes_peak=N dir_opens=N memfs.opens=N memfs.distinct=N ...
void FsStatReport(const char *why) {
  rt::RuntimeLibcGuard g;
  std::ostringstream ss;
  ss << "fsstat: why=" << why << " t="
     << Duration::Since(start_time).Microseconds() / 1e6
     << " memfs_files=" << memfs_files.load()
     << " memfs_bytes=" << memfs_bytes.load()
     << " memfs_files_peak=" << memfs_files_peak.load()
     << " memfs_bytes_peak=" << memfs_bytes_peak.load()
     << " dir_opens=" << dir_opens.load();
  for (int i = 0; i < kFsKinds; i++) {
    const KindCounters &k = kinds[i];
    const char *n = kind_names[i];
    ss << " " << n << ".opens=" << k.opens.load() << " " << n
       << ".distinct=" << k.distinct.load() << " " << n
       << ".reopened=" << k.reopened.load() << " " << n
       << ".reads=" << k.reads.load() << " " << n
       << ".read_bytes=" << k.read_bytes.load() << " " << n
       << ".writes=" << k.writes.load() << " " << n
       << ".write_bytes=" << k.write_bytes.load();
  }
  LOG(INFO) << ss.str();
}

void StartFsStatReporter() {
  start_time = Time::Now();
  rt::Spawn([] {
    while (true) {
      rt::Sleep(Duration(kFsStatPeriodUs));
      FsStatReport("periodic");
    }
  });
}

}  // namespace junction
