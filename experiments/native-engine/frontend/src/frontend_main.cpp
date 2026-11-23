// mu-native-gfx: the native-engine runner with a picture. The runner itself is the unchanged
// runner/runner_main.cpp, compiled with its main renamed; this file takes the frontend's own
// arguments out of the command line and hands the rest to it.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
extern "C" {
// The runner prints through libnx's text console, which would take the display Aurora draws on:
// the linker sends those three calls here instead (-Wl,--wrap). Output still goes to nxlink and the log files.
PrintConsole* __wrap_consoleInit(PrintConsole*) { return nullptr; }
void __wrap_consoleUpdate(PrintConsole*) {}
void __wrap_consoleExit(PrintConsole*) {}
}
#endif

int mu_runner_main(int argc, char** argv);

int main(int argc, char** argv) {
#ifdef __SWITCH__
  fe::options.out_dir = "sdmc:/switch/mu-native";
#else
  fe::options.out_dir = "./mu-native-out";
#endif
  std::vector<char*> rest;
  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (i == 0) rest.push_back(argv[i]);
    else if (arg == "--headless") fe::options.picture = false;
    else if (arg == "--unpaced") fe::options.pace = false;
    else if (arg == "--novsync") fe::options.vsync = false;
    else if (arg == "--gfx-null") fe::options.null_backend = true;
    else if (arg == "--gfx-log") fe::options.verbose = true;
    else if (arg == "--stretch") fe::options.stretch = true;
    else if (arg == "--gfx-sync") fe::options.sync_decode = true;
    else if (arg == "--warm-all") fe::options.warm_all = true;
    else if (arg == "--window" && i + 1 < argc) { std::sscanf(argv[++i], "%dx%d", &fe::options.width, &fe::options.height); }
    else {
      if (arg == "--out" && i + 1 < argc) fe::options.out_dir = argv[i + 1];
      if (arg == "--no-overclock") fe::options.overclock = false;
      rest.push_back(argv[i]);
    }
  }
  rest.push_back(nullptr);
  const int result = mu_runner_main(int(rest.size()) - 1, rest.data());
#ifdef __SWITCH__
  // As the app does (switch/src/switch_threads.c): leave by asking the system to close the application.
  // Returning to the Homebrew Menu after a graphics session left helper threads behind in the same process.
  if (fe::options.picture) std::exit(result);
#endif
  return result;
}
