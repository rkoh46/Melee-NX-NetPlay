// melee-unlocked's Lab view (port/runtime/gx/lab_view.cpp, compiled from the fetched tree by src/lab.cpp) reads a few host
// services; build.sh sync renames them in its copy (host::options -> host::lab_options and so on) to these stand-ins.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
namespace host {
void log(const char* format, ...);                 // the frontend's shim (src/shim/host.h): picture.log
struct LabOptions { std::string lab_dir; bool spectate = false; };
extern LabOptions lab_options;                     // lab_dir: <app folder>/Lab, the fighter packs
extern uint8_t* lab_ram;                           // null: the Source Port's view reads the game's snapshot, never guest memory
float lab_window_aspect();                         // the picture's shape (window width / height)
}  // namespace host
