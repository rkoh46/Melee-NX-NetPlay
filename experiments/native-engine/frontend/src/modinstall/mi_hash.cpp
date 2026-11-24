// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_hash.h"

#include <cctype>
#include <cstring>

namespace modinstall {
namespace {
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
const uint32_t kK256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
  0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
  0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
const uint32_t kMd5K[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
  0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
  0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
  0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
  0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
  0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
uint32_t g_crc_table[256];
bool g_crc_ready = [] {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc_table[i] = c;
  }
  return true;
}();
}  // namespace

std::string to_hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Sha256::Sha256() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(h_, init, sizeof h_);
}
void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK256[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}
void Sha256::update(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  total_ += size;
  if (fill_) {
    const size_t take = size < 64 - fill_ ? size : 64 - fill_;
    std::memcpy(buf_ + fill_, p, take);
    fill_ += take; p += take; size -= take;
    if (fill_ < 64) return;
    block(buf_);
    fill_ = 0;
  }
  while (size >= 64) { block(p); p += 64; size -= 64; }
  if (size) { std::memcpy(buf_, p, size); fill_ = size; }
}
std::string Sha256::hex() {
  const uint64_t bits = total_ * 8;
  const uint8_t one = 0x80, zero = 0;
  update(&one, 1);
  while (fill_ != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
  update(len, 8);
  uint8_t out[32];
  for (int i = 0; i < 8; ++i) { out[4 * i] = uint8_t(h_[i] >> 24); out[4 * i + 1] = uint8_t(h_[i] >> 16); out[4 * i + 2] = uint8_t(h_[i] >> 8); out[4 * i + 3] = uint8_t(h_[i]); }
  return to_hex(out, 32);
}

Md5::Md5() { h_[0] = 0x67452301; h_[1] = 0xefcdab89; h_[2] = 0x98badcfe; h_[3] = 0x10325476; }
void Md5::block(const uint8_t* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 | uint32_t(p[4 * i + 3]) << 24;
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f; int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | ~d); g = (7 * i) & 15; }
    const uint32_t t = d;
    d = c; c = b;
    b = b + rotl(a + f + kMd5K[i] + m[g], kMd5S[i]);
    a = t;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
}
void Md5::update(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  total_ += size;
  if (fill_) {
    const size_t take = size < 64 - fill_ ? size : 64 - fill_;
    std::memcpy(buf_ + fill_, p, take);
    fill_ += take; p += take; size -= take;
    if (fill_ < 64) return;
    block(buf_);
    fill_ = 0;
  }
  while (size >= 64) { block(p); p += 64; size -= 64; }
  if (size) { std::memcpy(buf_, p, size); fill_ = size; }
}
std::string Md5::hex() {
  const uint64_t bits = total_ * 8;
  const uint8_t one = 0x80, zero = 0;
  update(&one, 1);
  while (fill_ != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (8 * i));
  update(len, 8);
  uint8_t out[16];
  for (int i = 0; i < 4; ++i) { out[4 * i] = uint8_t(h_[i]); out[4 * i + 1] = uint8_t(h_[i] >> 8); out[4 * i + 2] = uint8_t(h_[i] >> 16); out[4 * i + 3] = uint8_t(h_[i] >> 24); }
  return to_hex(out, 16);
}

uint32_t crc32_update(uint32_t crc, const void* data, size_t size) {
  (void)g_crc_ready;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  while (size--) crc = g_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
uint32_t adler32_update(uint32_t adler, const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t a = adler & 0xFFFF, b = adler >> 16;
  while (size) {
    size_t n = size < 5552 ? size : 5552;
    size -= n;
    while (n--) { a += *p++; b += a; }
    a %= 65521; b %= 65521;
  }
  return b << 16 | a;
}

}  // namespace modinstall
