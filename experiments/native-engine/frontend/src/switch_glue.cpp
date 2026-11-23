// Symbols the app's Switch layer (switch/src) and Aurora's Switch patches expect from their host program.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include <cstdarg>
#include <cstdio>

// switch/src/perf_switch.cpp reports through this.
extern "C" void pc_log_line(const char* fmt, ...) {
  char line[512];
  va_list args; va_start(args, fmt); std::vsnprintf(line, sizeof line, fmt, args); va_end(args);
  fe::say("%s", line);
}

// The app's sampling profiler (switch/src/switch_profile.cpp) is not part of this program.
extern "C" void switch_profile_second_thread_here(void) {}
