// dev.h - character and block device support

#pragma once

extern "C" {
#include <sys/stat.h>
}

#include <memory>

#include "junction/base/error.h"
#include "junction/fs/file.h"

namespace junction {

// Device numbers are kept in the encoding user space sees in st_rdev and
// passes to mknod() -- glibc's makedev() -- so that they can cross the system
// call boundary unchanged. (They used to be major << 20 | minor, the kernel's
// internal form, which stat() then reported as is: glibc's daemon() checks that
// /dev/null really is device 1,3 and refused to detach, which is how sshd
// failed to start.)
constexpr dev_t DeviceMajor(dev_t dev) {
  return ((dev >> 8) & 0xfff) | ((dev >> 32) & ~0xfffULL);
}
constexpr dev_t DeviceMinor(dev_t dev) {
  return (dev & 0xff) | ((dev >> 12) & ~0xffULL);
}
constexpr dev_t MakeDevice(dev_t major, dev_t minor) {
  return ((major & 0xfffULL) << 8) | ((major & ~0xfffULL) << 32) |
         (minor & 0xffULL) | ((minor & ~0xffULL) << 12);
}

// DeviceOpen creates a special file for the inode's device number.
Status<std::shared_ptr<File>> DeviceOpen(std::shared_ptr<DirectoryEntry> dent,
                                         dev_t dev, unsigned int flags,
                                         FileMode mode);

}  // namespace junction
