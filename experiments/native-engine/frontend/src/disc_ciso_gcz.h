// CISO and GCZ disc images converted once into a plain image (src/disc_ciso_gcz.cpp; app.cpp app_disc_convert).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace fe::disc_ciso_gcz {
enum class Format { None, Ciso, Gcz };
Format detect(const std::string& path);
using Progress = std::function<bool(uint64_t done, uint64_t total)>;   // false stops
// Writes `target` (through target.part, renamed when complete). False with the reason in *error.
bool convert(const std::string& source, const std::string& target, const Progress& progress, std::string* error);
}  // namespace fe::disc_ciso_gcz
