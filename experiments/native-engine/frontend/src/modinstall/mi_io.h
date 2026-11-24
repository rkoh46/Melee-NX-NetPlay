// Byte streams, files and the two things the installer asks of the platform (free space, HTTPS).
// Plain C++ and POSIX file calls (fopen/fseeko, stat, rename, mkdir), which the Switch's newlib, Linux and macOS
// all have. Everything that differs between platforms is behind `Platform` and `Fetcher`.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace modinstall {

// A forward-only stream of bytes: a file, a zip entry being inflated, a 7z folder being decoded.
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Up to `size` bytes; 0 at the end; -1 on an error (error() says which).
  virtual long read(uint8_t* out, size_t size) = 0;
  virtual uint64_t total() const { return 0; }         // the stream's length when known (progress), else 0
  virtual uint64_t consumed() const { return pos_; }
  virtual std::string error() const { return error_; }
 protected:
  uint64_t pos_ = 0;
  std::string error_;
};

// A file, read with a large buffer.
class FileSource : public ByteSource {
 public:
  explicit FileSource(const std::string& path, uint64_t offset = 0, uint64_t length = UINT64_MAX);
  ~FileSource() override;
  bool ok() const { return file_ != nullptr; }
  long read(uint8_t* out, size_t size) override;
  uint64_t total() const override { return length_; }
 private:
  FILE* file_ = nullptr;
  uint64_t length_ = 0;
};

// Random reads of the player's disc (the patch source), through a few cached blocks.
class RandomFile {
 public:
  explicit RandomFile(const std::string& path);
  ~RandomFile();
  bool ok() const { return file_ != nullptr; }
  uint64_t size() const { return size_; }
  bool read(uint64_t offset, uint8_t* out, size_t size);
 private:
  struct Block { uint64_t index = UINT64_MAX; uint64_t used = 0; std::vector<uint8_t> data; size_t length = 0; };
  static constexpr size_t kBlock = 1u << 20, kBlocks = 8;
  FILE* file_ = nullptr;
  uint64_t size_ = 0, clock_ = 0;
  Block blocks_[kBlocks];
};

// Files.
bool file_exists(const std::string& path);
bool dir_exists(const std::string& path);
uint64_t file_size(const std::string& path);         // 0 when missing
int64_t file_mtime(const std::string& path);
bool make_dirs(const std::string& path);              // like mkdir -p
bool remove_file(const std::string& path);
bool remove_dir_if_empty(const std::string& path);
bool replace_file(const std::string& from, const std::string& to);   // rename over `to` (FAT: remove first)
bool write_text(const std::string& path, const std::string& text);   // via a temporary and a rename
bool read_text(const std::string& path, std::string* text, size_t limit = 1u << 20);
std::vector<std::string> list_dir(const std::string& path);         // names, no "." or ".."
std::string join(const std::string& a, const std::string& b);
std::string base_name(const std::string& path);
// The SHA-256 of a file (empty when it cannot be read); `progress(done, total)` may return false to stop.
std::string sha256_file(const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress = nullptr);
std::string md5_file(const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress = nullptr);

// What the installer asks of the platform.
class Platform {
 public:
  virtual ~Platform() = default;
  // Free bytes on the volume holding `path`; UINT64_MAX when it cannot be told (the install then goes ahead and
  // stops on the first failed write instead).
  virtual uint64_t free_bytes(const std::string& path);
};

// HTTPS downloads (the app's libcurl on the console and on Linux, the system's on macOS: mi_fetch_curl.cpp).
class Fetcher {
 public:
  virtual ~Fetcher() = default;
  struct Result { bool ok = false; bool cancelled = false; uint64_t bytes = 0; std::string error; };
  // Downloads `url` to `path`, continuing a partial file already at `path` (an HTTP range request; a server that
  // answers with the whole file starts it again). Redirects are followed to https only. `expected_size`, when
  // nonzero, is the file's exact size: more is refused. `progress(done, total)` returns false to cancel.
  virtual Result fetch(const std::string& url, const std::string& path, uint64_t expected_size,
                       const std::function<bool(uint64_t, uint64_t)>& progress) = 0;
};

}  // namespace modinstall
