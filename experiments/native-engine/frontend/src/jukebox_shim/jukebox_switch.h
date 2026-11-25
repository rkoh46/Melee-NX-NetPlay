// What jukebox.cpp (patches/0118) needs in place of <windows.h>: the MusicPacks folder (the app's folder on the SD card,
// features/audio.cpp) and the Windows names it uses for a case-blind compare and a seed.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <strings.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
inline uint32_t GetTickCount() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace jukebox_switch {
std::filesystem::path music_packs_root();   // empty: none (no app folder)
}
