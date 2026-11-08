// Platform-neutral file operations used by the disc loader.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#ifndef _WIN32
#include <sys/types.h>
#endif

namespace host {
inline bool seek_file(FILE* file, uint64_t offset) {
#ifdef _WIN32
  if (offset > uint64_t(std::numeric_limits<int64_t>::max())) return false;
  return _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
  if (offset > uint64_t(std::numeric_limits<off_t>::max())) return false;
  return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

// Names in the GameCube FST are ASCII. A missing terminator is invalid, even when
// the remaining bytes happen to be a prefix of the requested name.
inline bool fst_name_equal(const char* name, size_t available, std::string_view wanted, bool ignore_case) {
  const char* end = static_cast<const char*>(std::memchr(name, 0, available));
  if (!end || size_t(end - name) != wanted.size()) return false;
  auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
  for (size_t i = 0; i < wanted.size(); ++i) {
    unsigned char a = static_cast<unsigned char>(name[i]), b = static_cast<unsigned char>(wanted[i]);
    if (ignore_case ? fold(a) != fold(b) : a != b) return false;
  }
  return true;
}
}  // namespace host
