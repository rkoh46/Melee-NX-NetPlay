// CISO and GCZ disc images written out as the plain image the app plays from (melee-unlocked only detects them and asks for a
// conversion in Dolphin: its "CISO, GCZ" row is a stub). The two formats as Dolphin reads them (Source/Core/DiscIO/CISOBlob.cpp,
// CompressedBlob.cpp; formats described, no code copied):
//   CISO  "CISO", u32 block size (little endian), 0x8000 - 8 bytes of block map (1: the block is stored, in order, after the
//         0x8000-byte header; 0: a block of zeros).
//   GCZ   u32 magic 0xB10BC001, u32 sub type, u64 compressed data size, u64 image size, u32 block size, u32 block count (little
//         endian), then u64 block offsets (bit 63 set: stored uncompressed) and u32 Adler-32 hashes, then the blocks, each a zlib
//         stream unless stored.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "disc_ciso_gcz.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <zlib.h>

namespace fe::disc_ciso_gcz {
namespace {
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
struct File { FILE* f = nullptr; ~File() { if (f) std::fclose(f); } };
bool read_at(FILE* f, uint64_t at, void* out, size_t n) { return fseeko(f, off_t(at), SEEK_SET) == 0 && std::fread(out, 1, n, f) == n; }
bool fail(std::string* error, const std::string& why) { if (error) *error = why; return false; }
}  // namespace

Format detect(const std::string& path) {
  File in; in.f = std::fopen(path.c_str(), "rb");
  uint8_t head[4];
  if (!in.f || std::fread(head, 1, 4, in.f) != 4) return Format::None;
  if (std::memcmp(head, "CISO", 4) == 0) return Format::Ciso;
  if (le32(head) == 0xB10BC001u) return Format::Gcz;
  return Format::None;
}

bool convert(const std::string& source, const std::string& target, const Progress& progress, std::string* error) {
  const Format format = detect(source);
  if (format == Format::None) return fail(error, "not a CISO or GCZ image");
  File in; in.f = std::fopen(source.c_str(), "rb");
  if (!in.f) return fail(error, "cannot open " + source);
  const std::string partial = target + ".part";
  File out; out.f = std::fopen(partial.c_str(), "wb");
  if (!out.f) return fail(error, "cannot write " + partial);
  bool ok = true;
  if (format == Format::Ciso) {
    std::vector<uint8_t> header(0x8000);
    if (!read_at(in.f, 0, header.data(), header.size())) return fail(error, "short CISO header");
    const uint32_t block = le32(&header[4]);
    if (block == 0 || block > (64u << 20) || (block & (block - 1))) return fail(error, "bad CISO block size");
    const uint8_t* map = &header[8];
    const size_t blocks = header.size() - 8;
    size_t last = 0;
    for (size_t i = 0; i < blocks; ++i) if (map[i]) last = i + 1;   // the image ends with its last stored block
    std::vector<uint8_t> buf(block), zeros(block, 0);
    uint64_t stored = 0;
    for (size_t i = 0; i < last && ok; ++i) {
      if (map[i]) {
        if (!read_at(in.f, 0x8000 + stored * block, buf.data(), block)) { ok = fail(error, "CISO block " + std::to_string(i) + " cut short"); break; }
        ++stored;
        ok = std::fwrite(buf.data(), 1, block, out.f) == block;
      } else ok = std::fwrite(zeros.data(), 1, block, out.f) == block;
      if (progress && (i & 63) == 0 && !progress(i, last)) ok = fail(error, "stopped");
    }
  } else {
    uint8_t head[32];
    if (!read_at(in.f, 0, head, sizeof head)) return fail(error, "short GCZ header");
    const uint64_t data_size = le64(head + 16);
    const uint32_t block = le32(head + 24), count = le32(head + 28);
    if (block == 0 || block > (64u << 20) || count == 0 || uint64_t(count) * block < data_size) return fail(error, "bad GCZ header");
    std::vector<uint8_t> table(size_t(count) * 12);
    if (!read_at(in.f, 32, table.data(), table.size())) return fail(error, "short GCZ block table");
    const uint64_t data_start = 32 + uint64_t(count) * 12;
    std::vector<uint8_t> packed, plain(block);
    for (uint32_t i = 0; i < count && ok; ++i) {
      const uint64_t raw = le64(&table[size_t(i) * 8]);
      const bool stored = raw >> 63;
      const uint64_t at = raw & ~(1ull << 63);
      uint64_t end = i + 1 < count ? (le64(&table[size_t(i + 1) * 8]) & ~(1ull << 63)) : le64(head + 8);
      if (end < at || end - at > uint64_t(block) * 2 + 64) { ok = fail(error, "bad GCZ block " + std::to_string(i)); break; }
      packed.resize(size_t(end - at));
      if (!read_at(in.f, data_start + at, packed.data(), packed.size())) { ok = fail(error, "GCZ block " + std::to_string(i) + " cut short"); break; }
      const uint32_t expect = le32(&table[size_t(count) * 8 + size_t(i) * 4]);
      if (adler32(1, packed.data(), uInt(packed.size())) != expect) { ok = fail(error, "GCZ block " + std::to_string(i) + " fails its check"); break; }
      const uint64_t left = data_size - uint64_t(i) * block;
      const size_t want = size_t(left < block ? left : block);
      if (stored) {
        if (packed.size() < want) { ok = fail(error, "stored GCZ block too short"); break; }
        std::memcpy(plain.data(), packed.data(), want);
      } else {
        uLongf got = uLongf(block);
        if (uncompress(plain.data(), &got, packed.data(), uLong(packed.size())) != Z_OK || got < want) { ok = fail(error, "GCZ block " + std::to_string(i) + " does not unpack"); break; }
      }
      ok = std::fwrite(plain.data(), 1, want, out.f) == want;
      if (progress && (i & 63) == 0 && !progress(i, count)) ok = fail(error, "stopped");
    }
  }
  if (std::fclose(out.f) != 0) ok = false;
  out.f = nullptr;
  if (!ok) { std::remove(partial.c_str()); if (error && error->empty()) *error = "write failed"; return false; }
  if (std::rename(partial.c_str(), target.c_str()) != 0) { std::remove(partial.c_str()); return fail(error, "cannot rename to " + target); }
  return true;
}
}  // namespace fe::disc_ciso_gcz
