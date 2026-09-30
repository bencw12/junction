// fsstat.h - filesystem usage counters for density experiments.
//
// What a sandbox does to its filesystem, seen from the one place that sees all
// of it: how many distinct files it opens, how many it comes back to, how many
// bytes it reads and writes through each backing store, and how much memfs
// (the in-memory /tmp and /dev) it holds. A runtime thread logs a snapshot
// every kFsStatPeriodUs ("fsstat: ...", parsed by the replay analysis), and a
// last one when init exits.
#pragma once

#include <cstddef>
#include <cstdint>

namespace junction {

class File;
class Inode;

enum FsKind : int { kFsMemfs = 0, kFsLinuxfs = 1, kFsOther = 2, kFsKinds = 3 };

void FsStatNoteOpen(Inode &ino);
void FsStatNoteIo(const File *f, size_t bytes, bool is_write);
void FsStatMemfsFiles(int delta);
void FsStatMemfsBytes(int64_t delta);
void FsStatReport(const char *why);
void StartFsStatReporter();

}  // namespace junction
