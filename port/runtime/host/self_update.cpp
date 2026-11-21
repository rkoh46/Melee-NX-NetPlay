// See self_update.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "self_update.h"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace host::self_update {
namespace {
using json = nlohmann::json;

bool parse_version(const std::string& text, int out[3]) {
  size_t pos = !text.empty() && (text[0] == 'v' || text[0] == 'V') ? 1 : 0;
  for (int i = 0; i < 3; ++i) {
    if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return false;
    long value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { value = value * 10 + (text[pos++] - '0'); if (value > 1000000) return false; }
    out[i] = (int)value;
    if (i < 2) { if (pos >= text.size() || text[pos] != '.') return false; ++pos; }
  }
  return pos == text.size();
}

bool plain_nro_name(const std::string& name) {
  if (name.size() < 5 || name.size() > 96 || name[0] == '.') return false;
  for (unsigned char c : name)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
  return name.compare(name.size() - 4, 4, ".nro") == 0 && name.find("..") == std::string::npos;
}
bool hex64(const std::string& text) {
  if (text.size() != 64) return false;
  for (char c : text) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
std::string text_field(const json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}
bool file_size(const std::string& path, uint64_t& size) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  size = (uint64_t)st.st_size;
  return true;
}
bool exists(const std::string& path) { struct stat st; return stat(path.c_str(), &st) == 0; }
std::string read_small(const std::string& path) {
  std::string out;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buffer[256];
    const size_t n = std::fread(buffer, 1, sizeof buffer, f);
    std::fclose(f);
    out.assign(buffer, n);
  }
  return out;
}
constexpr uint32_t kRound[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
inline uint32_t rotate(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
}  // namespace

Sha256::Sha256() : state_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19} {}
void Sha256::block(const uint8_t* bytes) {
  uint32_t words[64];
  for (unsigned i = 0; i < 16; ++i) {
    const uint8_t* p = bytes + i * 4;
    words[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  for (unsigned i = 16; i < 64; ++i) {
    const uint32_t a = words[i - 15], b = words[i - 2];
    words[i] = words[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) + words[i - 7] + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (unsigned i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + words[i];
    const uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}
void Sha256::update(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  total_ += size;
  if (buffered_) {
    const size_t take = size < 64 - buffered_ ? size : 64 - buffered_;
    std::memcpy(buffer_ + buffered_, p, take);
    buffered_ += take; p += take; size -= take;
    if (buffered_ < 64) return;
    block(buffer_); buffered_ = 0;
  }
  while (size >= 64) { block(p); p += 64; size -= 64; }
  if (size) { std::memcpy(buffer_, p, size); buffered_ = size; }
}
std::string Sha256::hex() {
  const uint64_t bits = total_ * 8;
  uint8_t tail[128] = {};
  std::memcpy(tail, buffer_, buffered_);
  tail[buffered_] = 0x80;
  const size_t padded = buffered_ < 56 ? 64 : 128;
  for (unsigned i = 0; i < 8; ++i) tail[padded - 1 - i] = uint8_t(bits >> (i * 8));
  block(tail);
  if (padded == 128) block(tail + 64);
  std::string result;
  for (unsigned i = 0; i < 32; ++i) {
    const uint8_t byte = uint8_t(state_[i / 4] >> (24 - (i % 4) * 8));
    result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

bool parse_manifest(const std::string& text, Manifest& out, std::string& error) {
  out = Manifest();
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) { error = "the update list is not valid JSON"; return false; }
  const auto schema = j.find("schema");
  if (schema == j.end() || !schema->is_number_integer() || schema->get<int>() != 1) { error = "the update list has an unknown format"; return false; }
  out.schema = 1;
  out.app_version = text_field(j, "app_version");
  out.slippi_version = text_field(j, "slippi_version");
  out.built = text_field(j, "built");
  out.commit = text_field(j, "commit");
  int parts[3];
  if (!parse_version(out.app_version, parts) || !parse_version(out.slippi_version, parts)) { error = "the update list has no usable version"; return false; }
  const auto files = j.find("files");
  if (files == j.end() || !files->is_object()) { error = "the update list names no files"; return false; }
  for (auto it = files->begin(); it != files->end(); ++it) {
    if (!it.value().is_object()) { error = "the update list has a malformed file entry"; return false; }
    FileEntry entry;
    entry.name = text_field(it.value(), "name");
    entry.sha256 = text_field(it.value(), "sha256");
    const auto size = it.value().find("size");
    if (size == it.value().end() || !size->is_number_unsigned()) { error = "the update list has a file without a size"; return false; }
    entry.size = size->get<uint64_t>();
    // An NRO of this app is tens of megabytes; anything else is not one.
    if (!plain_nro_name(entry.name) || !hex64(entry.sha256) || entry.size < 1024 * 1024 || entry.size > 512ull * 1024 * 1024) {
      error = "the update list has a file entry that cannot be right"; return false;
    }
    out.files[it.key()] = entry;
  }
  if (!out.files.count("game")) { error = "the update list does not name the game"; return false; }
  const auto pending = j.find("pending");
  if (pending != j.end() && pending->is_object()) {
    out.pending_slippi = text_field(*pending, "slippi");
    out.pending_reason = text_field(*pending, "reason");
    if (!parse_version(out.pending_slippi, parts)) { out.pending_slippi.clear(); out.pending_reason.clear(); }
  }
  error.clear();
  return true;
}

bool version_newer(const std::string& theirs, const std::string& ours) {
  int a[3], b[3];
  if (!parse_version(theirs, a) || !parse_version(ours, b)) return false;
  for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] > b[i];
  return false;
}

bool should_offer(const Manifest& manifest, const std::string& app_version, const std::string& slippi_version) {
  if (manifest.schema != 1) return false;
  if (version_newer(manifest.app_version, app_version)) return !version_newer(slippi_version, manifest.slippi_version);   // never back to an older Slippi
  if (version_newer(app_version, manifest.app_version)) return false;
  return version_newer(manifest.slippi_version, slippi_version);
}

std::string join_url(const std::string& host, const std::string& name) {
  std::string base = host;
  while (!base.empty() && (base.back() == ' ' || base.back() == '\r' || base.back() == '\t')) base.pop_back();
  if (base.compare(0, 7, "http://") != 0 && base.compare(0, 8, "https://") != 0) return {};
  if (base.size() <= (base[4] == 's' ? 8u : 7u)) return {};
  for (unsigned char c : base) if (c <= ' ' || c >= 127) return {};
  while (base.back() == '/') base.pop_back();
  return base + "/" + name;
}

std::string staged_path(const std::string& nro) { return nro + ".new"; }
std::string marker_path(const std::string& nro) { return nro + ".new.ok"; }
std::string backup_path(const std::string& nro) { return nro + ".bak"; }

bool download_verified(const Fetch& fetch, const std::string& url, const FileEntry& entry, const std::string& nro,
                       const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
  const std::string staged = staged_path(nro), marker = marker_path(nro);
  std::remove(marker.c_str());   // first: from here on nothing claims the staged file is good
  std::remove(staged.c_str());
  FILE* file = std::fopen(staged.c_str(), "wb");
  if (!file) { error = "cannot write beside the installed app (is the SD card full or locked?)"; return false; }
  Sha256 hash;
  uint64_t done = 0;
  bool write_failed = false, too_long = false, cancelled = false;
  std::string fetch_error;
  const bool fetched = fetch(url, [&](const void* data, size_t size) {
    if (done + size > entry.size) { too_long = true; return false; }
    if (std::fwrite(data, 1, size, file) != size) { write_failed = true; return false; }
    hash.update(data, size);
    done += size;
    if (progress && !progress(done, entry.size)) { cancelled = true; return false; }
    return true;
  }, fetch_error);
  const bool closed = std::fflush(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  uint64_t on_disk = 0;
  if (cancelled) error = "the update was cancelled";
  else if (write_failed || !closed || !close_ok) error = "could not write the update to the SD card (is it full?)";
  else if (too_long) error = "the download is larger than the update list says";
  else if (!fetched) error = fetch_error.empty() ? "the download failed" : fetch_error;
  else if (done != entry.size) error = "the download stopped early";
  else if (hash.hex() != entry.sha256) error = "the downloaded file does not match its checksum";
  else if (!file_size(staged, on_disk) || on_disk != entry.size) error = "the downloaded file did not reach the SD card whole";
  else {
    // The marker is what makes the staged file eligible for the swap: it is written last and names the size.
    const std::string note = std::to_string(entry.size) + " " + entry.sha256 + "\n";
    FILE* m = std::fopen(marker.c_str(), "wb");
    const bool marked = m && std::fwrite(note.data(), 1, note.size(), m) == note.size();
    if (m && std::fclose(m) != 0) { std::remove(marker.c_str()); error = "could not finish writing the update"; std::remove(staged.c_str()); return false; }
    if (marked) { error.clear(); return true; }
    error = "could not finish writing the update";
  }
  std::remove(marker.c_str());
  std::remove(staged.c_str());
  return false;
}

Swap apply_pending(const std::string& nro, std::string& error) {
  const std::string staged = staged_path(nro), marker = marker_path(nro), backup = backup_path(nro);
  error.clear();
  if (!exists(marker)) {
    if (exists(staged)) std::remove(staged.c_str());   // an interrupted download: never eligible
    return Swap::Nothing;
  }
  uint64_t staged_size = 0;
  const std::string note = read_small(marker);
  const uint64_t marked_size = std::strtoull(note.c_str(), nullptr, 10);
  if (!file_size(staged, staged_size) || marked_size == 0 || staged_size != marked_size) {
    std::remove(marker.c_str());
    std::remove(staged.c_str());
    error = "the waiting update was incomplete and has been removed";
    return Swap::Nothing;
  }
  const bool had_installed = exists(nro);
  if (had_installed) {
    std::remove(backup.c_str());
    if (std::rename(nro.c_str(), backup.c_str()) != 0) {
      error = std::string("the installed app could not be moved aside (") + std::strerror(errno) + ")";
      return Swap::Failed;   // staged file and marker stay: tried again at the next launch
    }
  }
  if (std::rename(staged.c_str(), nro.c_str()) != 0) {
    error = std::string("the update could not be moved into place (") + std::strerror(errno) + ")";
    if (had_installed) std::rename(backup.c_str(), nro.c_str());   // put the installed app back
    return Swap::Failed;
  }
  std::remove(marker.c_str());
  return Swap::Done;
}
}  // namespace host::self_update
