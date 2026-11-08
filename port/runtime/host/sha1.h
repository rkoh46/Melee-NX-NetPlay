// SHA-1 for matching the pinned game image, not for authentication.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace host {
inline std::array<uint8_t, 20> sha1(const uint8_t* data, size_t size) {
  uint32_t state[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
  auto rotate = [](uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); };
  auto block = [&](const uint8_t* bytes) {
    uint32_t words[80];
    for (unsigned i = 0; i < 16; ++i) {
      const uint8_t* p = bytes + i * 4;
      words[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    for (unsigned i = 16; i < 80; ++i)
      words[i] = rotate(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (unsigned i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
      else { f = b ^ c ^ d; k = 0xca62c1d6u; }
      uint32_t next = rotate(a, 5) + f + e + k + words[i];
      e = d; d = c; c = rotate(b, 30); b = a; a = next;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
  };
  size_t remaining = size;
  while (remaining >= 64) { block(data); data += 64; remaining -= 64; }
  uint8_t tail[128] = {};
  if (remaining) std::memcpy(tail, data, remaining);
  tail[remaining] = 0x80;
  size_t padded = remaining < 56 ? 64 : 128;
  uint64_t bits = uint64_t(size) * 8;
  for (unsigned i = 0; i < 8; ++i) tail[padded - 1 - i] = uint8_t(bits >> (i * 8));
  block(tail);
  if (padded == 128) block(tail + 64);
  std::array<uint8_t, 20> result{};
  for (unsigned i = 0; i < 20; ++i) result[i] = uint8_t(state[i / 4] >> (24 - (i % 4) * 8));
  return result;
}
}  // namespace host
