// SHA-256, MD5, CRC-32 and Adler-32, streaming: the mod installer checks a download, a patch and a built disc
// while the bytes go by, so nothing is read twice and nothing large is held in memory.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace modinstall {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t size);
  std::string hex();          // finishes; call once
 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  uint64_t total_ = 0;
  size_t fill_ = 0;
};

class Md5 {
 public:
  Md5();
  void update(const void* data, size_t size);
  std::string hex();          // finishes; call once
 private:
  void block(const uint8_t* p);
  uint32_t h_[4];
  uint8_t buf_[64];
  uint64_t total_ = 0;
  size_t fill_ = 0;
};

uint32_t crc32_update(uint32_t crc, const void* data, size_t size);   // start with 0 (zip, 7z, xz)
uint32_t adler32_update(uint32_t adler, const void* data, size_t size);   // start with 1 (VCDIFF's VCD_ADLER32)

std::string to_hex(const uint8_t* p, size_t n);
std::string lower(std::string s);

}  // namespace modinstall
