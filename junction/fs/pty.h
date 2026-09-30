// pty.h - pseudo-terminals: /dev/ptmx, /dev/pts/N and /dev/tty

#pragma once

#include <memory>

#include "junction/base/error.h"
#include "junction/fs/file.h"
#include "junction/fs/fs.h"

namespace junction {

// Creates /dev/ptmx, /dev/tty and /dev/pts under @dev.
Status<void> SetupPtys(IDir &dev);

// Whether @dev is one of the terminal devices served here.
bool IsPtyDevice(dev_t dev);

// Opens the master (a new pair), a slave, or the controlling terminal.
Status<std::shared_ptr<File>> PtyOpen(std::shared_ptr<DirectoryEntry> dent,
                                      dev_t dev, unsigned int flags,
                                      FileMode mode);

}  // namespace junction
