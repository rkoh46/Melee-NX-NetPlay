// Picture frontend for the native-engine runner. See frontend.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace fe {
struct Options {
  bool picture = true;        // --headless: behave as the headless runner
  bool pace = true;           // --unpaced: do not wait for 60 Hz after each retrace
  bool vsync = true;          // --novsync
  bool null_backend = false;  // --gfx-null: decode and check every frame, draw nothing
  bool verbose = false;       // --gfx-log: Aurora's information messages too
  bool sync_decode = false;   // --gfx-sync: decode inside the game's flush instead of on the decoder thread
  bool warm_all = false;      // --warm-all: compile every pipeline in the cache at launch, not only those marked as drawn
  bool overclock = true;      // the runner's --no-overclock also keeps the GPU and memory clocks as they are
  bool stretch = false;       // --stretch: fill the 16:9 screen instead of showing the game's 4:3 picture
  int width = 1280, height = 720;
  std::string out_dir;        // the runner's --out
};
extern Options options;
void say(const char* format, ...);   // stdout (nxlink) and <out>/picture.log
}  // namespace fe
