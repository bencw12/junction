// memprobe.h - see memprobe.cc. Diagnostic, enabled by --debug_libos_escape.

#pragma once

namespace junction {

// Runs a battery of LibOS allocation shapes and logs where each landed
// relative to a nominal arena slot. Call from a LibOS context.
void RunLibOSEscapeProbe();

}  // namespace junction
