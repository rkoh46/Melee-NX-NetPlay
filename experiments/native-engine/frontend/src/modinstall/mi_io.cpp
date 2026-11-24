// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_io.h"
#include "mi_hash.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

namespace modinstall {
namespace {
bool seek(FILE* f, uint64_t offset) { return fseeko(f, off_t(offset), SEEK_SET) == 0; }
}  // namespace

FileSource::FileSource(const std::string& path, uint64_t offset, uint64_t length) {
  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) { error_ = "cannot open " + path; return; }
  std::setvbuf(file_, nullptr, _IOFBF, 1u << 18);
  const uint64_t size = file_size(path);
  if (offset > size || !seek(file_, offset)) { std::fclose(file_); file_ = nullptr; error_ = "cannot seek in " + path; return; }
  length_ = length == UINT64_MAX || offset + length > size ? size - offset : length;
}
FileSource::~FileSource() { if (file_) std::fclose(file_); }
long FileSource::read(uint8_t* out, size_t size) {
  if (!file_) return -1;
  if (size > length_ - pos_) size = size_t(length_ - pos_);
  if (!size) return 0;
  const size_t got = std::fread(out, 1, size, file_);
  if (got == 0) { error_ = "read error"; return -1; }
  pos_ += got;
  return long(got);
}

RandomFile::RandomFile(const std::string& path) {
  file_ = std::fopen(path.c_str(), "rb");
  if (file_) { std::setvbuf(file_, nullptr, _IONBF, 0); size_ = file_size(path); }
}
RandomFile::~RandomFile() { if (file_) std::fclose(file_); }
bool RandomFile::read(uint64_t offset, uint8_t* out, size_t size) {
  if (!file_ || offset > size_ || size > size_ - offset) return false;
  while (size) {
    const uint64_t index = offset / kBlock;
    Block* hit = nullptr;
    for (auto& b : blocks_) if (b.index == index) { hit = &b; break; }
    if (!hit) {
      hit = &blocks_[0];
      for (auto& b : blocks_) if (b.used < hit->used) hit = &b;
      if (hit->data.empty()) hit->data.resize(kBlock);
      const uint64_t start = index * kBlock;
      const size_t want = size_t(std::min<uint64_t>(kBlock, size_ - start));
      if (!seek(file_, start) || std::fread(hit->data.data(), 1, want, file_) != want) { hit->index = UINT64_MAX; return false; }
      hit->index = index; hit->length = want;
    }
    hit->used = ++clock_;
    const size_t at = size_t(offset - index * kBlock);
    const size_t take = std::min(size, hit->length - at);
    std::memcpy(out, hit->data.data() + at, take);
    out += take; offset += take; size -= take;
  }
  return true;
}

bool file_exists(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
bool dir_exists(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
uint64_t file_size(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0; }
int64_t file_mtime(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0; }
bool make_dirs(const std::string& path) {
  if (path.empty() || dir_exists(path)) return true;
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && path[slash - 1] != ':') make_dirs(path.substr(0, slash));
  return mkdir(path.c_str(), 0777) == 0 || dir_exists(path);
}
bool remove_file(const std::string& path) { return std::remove(path.c_str()) == 0 || !file_exists(path); }
bool remove_dir_if_empty(const std::string& path) { return rmdir(path.c_str()) == 0; }
bool replace_file(const std::string& from, const std::string& to) {
  if (std::rename(from.c_str(), to.c_str()) == 0) return true;
  // FAT and exFAT (the console's SD card) refuse to rename over an existing file.
  if (file_exists(to) && std::remove(to.c_str()) == 0) return std::rename(from.c_str(), to.c_str()) == 0;
  return false;
}
bool write_text(const std::string& path, const std::string& text) {
  const std::string temporary = path + ".tmp";
  FILE* f = std::fopen(temporary.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  if (std::fclose(f) != 0 || !ok) { std::remove(temporary.c_str()); return false; }
  if (!replace_file(temporary, path)) { std::remove(temporary.c_str()); return false; }
  return true;
}
bool read_text(const std::string& path, std::string* text, size_t limit) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  text->clear();
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    if (text->size() + n > limit) { std::fclose(f); return false; }
    text->append(buf, n);
  }
  std::fclose(f);
  return true;
}
std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> out;
  if (DIR* d = opendir(path.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      if (name != "." && name != "..") out.push_back(name);
    }
    closedir(d);
  }
  return out;
}
std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  return a.back() == '/' ? a + b : a + "/" + b;
}
std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

namespace {
template <class Hash>
std::string hash_file(const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  const uint64_t total = file_size(path);
  std::vector<uint8_t> buf(1u << 20);
  Hash h;
  uint64_t done = 0;
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    h.update(buf.data(), n);
    done += n;
    if (progress && !progress(done, total)) { std::fclose(f); return {}; }
  }
  const bool error = std::ferror(f) != 0;
  std::fclose(f);
  return error ? std::string() : h.hex();
}
}  // namespace
std::string sha256_file(const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress) { return hash_file<Sha256>(path, progress); }
std::string md5_file(const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress) { return hash_file<Md5>(path, progress); }

uint64_t Platform::free_bytes(const std::string& path) {
  struct statvfs v{};
  if (statvfs(path.c_str(), &v) != 0 || v.f_frsize == 0) return UINT64_MAX;
  return uint64_t(v.f_bavail) * uint64_t(v.f_frsize ? v.f_frsize : v.f_bsize);
}

}  // namespace modinstall
