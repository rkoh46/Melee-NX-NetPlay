// A mod disc's content fingerprint exactly as melee-unlocked computes it (port/app/source_host.cpp iso_layer_fingerprint:
// its ModOverlay, port/runtime/host/source_mod_overlay.cpp, compares the disc with the retail one file by file, and
// layer_fingerprint.h hashes what it changes, both compiled from the fetched tree), so the packs upstream knows by that
// fingerprint are known here too: the 2D-style m-ex build (its camera and HUD codes run natively, MU_GAME_OPTION2_ARCADE_PACK)
// and, as a check of the computation, Akaneia 1.0.1 and the others in its table. The retail disc is read through the host's
// disc table (patches/0101). SPDX-License-Identifier: GPL-3.0-or-later
#include "source_mod_overlay.h"
#include "layer_fingerprint.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace linux_host { bool fe_disc_read_base(uint32_t offset, void* out, uint32_t bytes); }   // patches/0101

namespace fe {
namespace {
// SHA-256, incremental (layer_fingerprint takes an object with update() and hex()).
class Sha256 {
 public:
  void update(const std::string& s) { update(s.data(), s.size()); }
  void update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    total_ += size;
    while (size) {
      const size_t take = std::min(size, size_t(64) - used_);
      std::memcpy(buffer_ + used_, p, take);
      used_ += take; p += take; size -= take;
      if (used_ == 64) { block(buffer_); used_ = 0; }
    }
  }
  std::string hex() {
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (used_ != 56) update(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
    update(length, 8);
    std::string out;
    for (uint32_t v : state_) for (int s = 24; s >= 0; s -= 8) { const uint8_t b = uint8_t(v >> s); out += "0123456789abcdef"[b >> 4]; out += "0123456789abcdef"[b & 15]; }
    return out;
  }
 private:
  static uint32_t rotr(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
  void block(const uint8_t* b) {
    static constexpr uint32_t k[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
      0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
      0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
      0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(b[i * 4]) << 24 | uint32_t(b[i * 4 + 1]) << 16 | uint32_t(b[i * 4 + 2]) << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; ++i)
      w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
    uint32_t a = state_[0], c1 = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & c1) ^ (a & c) ^ (c1 & c));
      h = g; g = f; f = e; e = d + t1; d = c; c = c1; c1 = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += c1; state_[2] += c; state_[3] += d; state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
  }
  uint32_t state_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buffer_[64] = {};
  size_t used_ = 0;
  uint64_t total_ = 0;
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool read_base(uint32_t offset, void* out, uint32_t size) { return linux_host::fe_disc_read_base(offset, out, size); }

// The retail disc's files (lower case, leading slash, as upstream's path table) and its main.dol's extent.
bool retail_table(std::map<std::string, source_port::ModOverlay::DiscFile>* files, source_port::ModOverlay::BaseDisc* base, std::string* error) {
  uint8_t header[0x440];
  if (!read_base(0, header, sizeof header)) { *error = "cannot read the retail disc"; return false; }
  const uint32_t fst_offset = be32(header + 0x424), fst_size = be32(header + 0x428);
  if (fst_size < 12 || fst_size > 2u * 1024 * 1024) { *error = "invalid retail filesystem table"; return false; }
  std::vector<uint8_t> fst(fst_size);
  if (!read_base(fst_offset, fst.data(), fst_size)) { *error = "cannot read the retail filesystem table"; return false; }
  const uint32_t count = be32(fst.data() + 8);
  if (count < 1 || count > 4096 || uint64_t(count) * 12 > fst_size) { *error = "invalid retail filesystem entries"; return false; }
  const size_t names = size_t(count) * 12;
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, ""}};
  for (uint32_t i = 1; i < count; ++i) {
    while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
    const uint8_t* entry = fst.data() + size_t(i) * 12;
    const uint32_t name_offset = be32(entry) & 0xFFFFFFu;
    if (name_offset >= fst_size - names) { *error = "invalid retail file name"; return false; }
    const char* begin = reinterpret_cast<const char*>(fst.data() + names + name_offset);
    const char* end = static_cast<const char*>(std::memchr(begin, 0, fst_size - names - name_offset));
    if (!end) { *error = "invalid retail file name"; return false; }
    std::string name(begin, end);
    for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    const std::string path = dirs.back().second + "/" + name;
    if (entry[0]) { dirs.push_back({be32(entry + 8), path}); continue; }
    (*files)[path] = {be32(entry + 4), be32(entry + 8)};
    base->files.push_back(path);
  }
  std::sort(base->files.begin(), base->files.end());
  uint8_t dol[0x100];
  if (read_base(be32(header + 0x420), dol, sizeof dol)) {
    base->dol_offset = be32(header + 0x420);
    uint32_t extent = sizeof dol;
    for (int i = 0; i < 18; ++i) extent = std::max(extent, be32(dol + i * 4) + be32(dol + 0x90 + i * 4));
    base->dol_size = extent;
  }
  return true;
}
}  // namespace

// The content fingerprint upstream gives a single mod disc layer, or "" with *error set.
std::string mods_layer_fingerprint(const std::string& iso, std::string* error) {
  std::map<std::string, source_port::ModOverlay::DiscFile> files;
  source_port::ModOverlay::BaseDisc base;
  if (!retail_table(&files, &base, error)) return {};
  source_port::ModOverlay overlay;
  overlay.set_base_disc(std::move(base), read_base);
  source_port::allow_native_akaneia(true);   // the native game has Akaneia's fighters (upstream's library says so too)
  const auto lookup = [&](const std::string& path, source_port::ModOverlay::DiscFile* out) {
    const auto it = files.find(path);
    if (it == files.end()) return false;
    *out = it->second;
    return true;
  };
  const std::string profile = "layer 1 " + iso;
  if (!overlay.add_iso(iso, profile, lookup, read_base, *error)) return {};
  return source_port::layer_fingerprint(overlay, profile, [] { return Sha256(); });
}
}  // namespace fe
