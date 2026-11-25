// The few host services melee-unlocked's jukebox.cpp (build-time copy, patches/0118) asks for, on this program: its log,
// the cost scope (not measured here), the worker guard (upstream's optional_work, as it is) and the disc reads, which
// features/audio.cpp answers (songs are read where the game reads them; the path of a song by its disc offset).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <vector>

namespace host {
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));
enum SimCost { SIM_JUKEBOX = 2 };
struct SimCostScope { explicit SimCostScope(int) {} };
// melee-unlocked port/runtime/host/host.h: the body of a worker whose work the game can do without.
template <class Body> void optional_work(const char* what, Body&& body) noexcept {
  try {
    body();
  } catch (const std::bad_alloc&) {
    log("%s: not enough memory; skipped", what);
  } catch (const std::exception& e) {
    log("%s: stopped (%s)", what, e.what());
  } catch (...) {
    log("%s: stopped", what);
  }
}
bool disc_read(uint32_t offset, void* dst, uint32_t size);
bool disc_find_path_by_offset(uint32_t offset, std::string* path);
bool disc_music_paths(std::vector<std::string>* paths);
}  // namespace host
