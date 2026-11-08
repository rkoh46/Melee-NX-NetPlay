// Names and paths accepted by Slippi's fixed-width guest game-file requests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace slippi::files {
inline bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > 63 || name.front() == '.' || name.back() == '.') return false;
  for (unsigned char c : name) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

inline std::optional<std::string> guest_name(const uint8_t* payload, size_t available) {
  if (!payload || available < 64) return std::nullopt;
  const auto* end = static_cast<const uint8_t*>(std::memchr(payload, 0, 64));
  if (!end) return std::nullopt;
  std::string name(reinterpret_cast<const char*>(payload), static_cast<size_t>(end - payload));
  if (!valid_name(name)) return std::nullopt;
  // The wire format is a C string in a 64-byte field. Bytes after its first NUL
  // are padding and never become part of a host path or a cache key.
  return name;
}

class GameFileRoot {
 public:
  // Capture the canonical Sys root once at EXI initialization. Changing host
  // options later cannot redirect a running game's resource reads.
  explicit GameFileRoot(const std::filesystem::path& sys) {
    std::error_code error;
    auto root = std::filesystem::canonical(sys, error);
    if (!error && std::filesystem::is_directory(root, error) && !error) root_ = std::move(root);
  }

  std::optional<std::filesystem::path> directory() const {
    if (root_.empty()) return std::nullopt;
    std::error_code error;
    auto dir = std::filesystem::canonical(root_ / "GameFiles" / "GALE01", error);
    if (error || !within_root(dir) || !std::filesystem::is_directory(dir, error) || error) return std::nullopt;
    return dir;
  }

  std::optional<std::filesystem::path> file(std::string_view name, bool diff) const {
    if (!valid_name(name)) return std::nullopt;
    const auto dir = directory();
    if (!dir) return std::nullopt;
    std::error_code error;
    auto path = std::filesystem::canonical(*dir / (std::string(name) + (diff ? ".diff" : "")), error);
    if (error || path.parent_path() != *dir || !std::filesystem::is_regular_file(path, error) || error)
      return std::nullopt;
    return path;
  }

 private:
  bool within_root(const std::filesystem::path& path) const {
    auto candidate = path.begin();
    for (const auto& component : root_) {
      if (candidate == path.end() || *candidate != component) return false;
      ++candidate;
    }
    return true;
  }
  std::filesystem::path root_;
};
}  // namespace slippi::files
