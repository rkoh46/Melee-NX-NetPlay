// Bridges between Melee-NX's Switch helpers and this runtime's logging.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.h"
#include <cstdarg>
#include <cstdio>

// perf_switch.cpp (from Melee-NX) reports through melee-pc's pc_log_line.
extern "C" void pc_log_line(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  host::log("%s", line);
}
