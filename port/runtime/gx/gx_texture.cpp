// GameCube texture decoding (block-tiled formats) to RGBA8.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_texture.h"
#include <cstring>

namespace gx {

namespace {
inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
inline uint8_t e5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
inline uint8_t e6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
inline uint8_t e4(uint32_t v) { return (uint8_t)(v * 17); }
inline uint8_t e3(uint32_t v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }

inline void rgb565(uint32_t c, uint8_t* o) { o[0] = e5(c >> 11); o[1] = e6((c >> 5) & 63); o[2] = e5(c & 31); o[3] = 255; }
inline void rgb5a3(uint32_t c, uint8_t* o) {
  if (c & 0x8000) { o[0] = e5((c >> 10) & 31); o[1] = e5((c >> 5) & 31); o[2] = e5(c & 31); o[3] = 255; }
  else { o[0] = e4((c >> 8) & 15); o[1] = e4((c >> 4) & 15); o[2] = e4(c & 15); o[3] = e3((c >> 12) & 7); }
}
inline void tlut_color(const uint8_t* tlut, uint32_t tlut_format, uint32_t index, uint8_t* o) {
  uint32_t c = be16(tlut + index * 2);
  switch (tlut_format) {
    case 0: o[0] = o[1] = o[2] = (uint8_t)(c & 0xFF); o[3] = (uint8_t)(c >> 8); break;   // IA8
    case 1: rgb565(c, o); break;
    default: rgb5a3(c, o); break;
  }
}
void block_dims(uint32_t format, uint32_t& bw, uint32_t& bh) {
  switch (format) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;
    case 1: case 2: case 9: bw = 8; bh = 4; break;
    default: bw = 4; bh = 4; break;
  }
}
}  // namespace

uint32_t texture_level_bytes(uint32_t width, uint32_t height, uint32_t format) {
  uint32_t bw, bh;
  block_dims(format, bw, bh);
  uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
  uint32_t block_bytes = (format == 6) ? 64 : 32;
  return bx * by * block_bytes;
}

uint64_t hash_bytes(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint64_t h = 0xcbf29ce484222325ull;
  size_t i = 0;
  for (; i + 8 <= len; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; }
  for (; i < len; ++i) { h = (h ^ p[i]) * 0x100000001b3ull; }
  return h;
}

void decode_texture(const uint8_t* src, uint32_t width, uint32_t height, uint32_t format,
                    const uint8_t* tlut, uint32_t tlut_format, std::vector<uint8_t>& out) {
  out.assign((size_t)width * height * 4, 0);
  auto put = [&](uint32_t x, uint32_t y, const uint8_t* c) {
    if (x < width && y < height) std::memcpy(&out[((size_t)y * width + x) * 4], c, 4);
  };
  uint32_t bw, bh;
  block_dims(format, bw, bh);
  const uint8_t* p = src;
  for (uint32_t by = 0; by < height; by += bh) {
    for (uint32_t bx = 0; bx < width; bx += bw) {
      uint8_t c[4];
      switch (format) {
        case 0:  // I4
          for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; x += 2) {
            uint8_t v = p[y * 4 + x / 2];
            c[0] = c[1] = c[2] = c[3] = e4(v >> 4); put(bx + x, by + y, c);
            c[0] = c[1] = c[2] = c[3] = e4(v & 15); put(bx + x + 1, by + y, c);
          }
          p += 32; break;
        case 1:  // I8
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) { uint8_t v = p[y * 8 + x]; c[0] = c[1] = c[2] = c[3] = v; put(bx + x, by + y, c); }
          p += 32; break;
        case 2:  // IA4
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) { uint8_t v = p[y * 8 + x]; c[0] = c[1] = c[2] = e4(v & 15); c[3] = e4(v >> 4); put(bx + x, by + y, c); }
          p += 32; break;
        case 3:  // IA8
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) { uint32_t v = be16(p + (y * 4 + x) * 2); c[0] = c[1] = c[2] = (uint8_t)(v & 0xFF); c[3] = (uint8_t)(v >> 8); put(bx + x, by + y, c); }
          p += 32; break;
        case 4:  // RGB565
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) { rgb565(be16(p + (y * 4 + x) * 2), c); put(bx + x, by + y, c); }
          p += 32; break;
        case 5:  // RGB5A3
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) { rgb5a3(be16(p + (y * 4 + x) * 2), c); put(bx + x, by + y, c); }
          p += 32; break;
        case 6:  // RGBA8: 32 bytes AR then 32 bytes GB
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
            const uint8_t* ar = p + (y * 4 + x) * 2; const uint8_t* gb = p + 32 + (y * 4 + x) * 2;
            c[0] = ar[1]; c[1] = gb[0]; c[2] = gb[1]; c[3] = ar[0]; put(bx + x, by + y, c);
          }
          p += 64; break;
        case 8:  // C4
          for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; x += 2) {
            uint8_t v = p[y * 4 + x / 2];
            tlut_color(tlut, tlut_format, v >> 4, c); put(bx + x, by + y, c);
            tlut_color(tlut, tlut_format, v & 15, c); put(bx + x + 1, by + y, c);
          }
          p += 32; break;
        case 9:  // C8
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) { tlut_color(tlut, tlut_format, p[y * 8 + x], c); put(bx + x, by + y, c); }
          p += 32; break;
        case 10: // C14X2
          for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) { tlut_color(tlut, tlut_format, be16(p + (y * 4 + x) * 2) & 0x3FFF, c); put(bx + x, by + y, c); }
          p += 32; break;
        case 14: {  // CMPR: 8x8 block = four 4x4 DXT1 sub-blocks
          for (uint32_t sub = 0; sub < 4; ++sub) {
            const uint8_t* q = p + sub * 8;
            uint32_t c0 = be16(q), c1 = be16(q + 2);
            uint8_t cols[4][4];
            rgb565(c0, cols[0]); rgb565(c1, cols[1]);
            if (c0 > c1) {
              for (int k = 0; k < 3; ++k) { cols[2][k] = (uint8_t)((2 * cols[0][k] + cols[1][k]) / 3); cols[3][k] = (uint8_t)((cols[0][k] + 2 * cols[1][k]) / 3); }
              cols[2][3] = cols[3][3] = 255;
            } else {
              for (int k = 0; k < 3; ++k) { cols[2][k] = (uint8_t)((cols[0][k] + cols[1][k]) / 2); cols[3][k] = cols[2][k]; }
              cols[2][3] = 255; cols[3][3] = 0;
            }
            uint32_t ox = bx + (sub & 1) * 4, oy = by + (sub >> 1) * 4;
            for (uint32_t y = 0; y < 4; ++y) {
              uint8_t row = q[4 + y];
              for (uint32_t x = 0; x < 4; ++x) put(ox + x, oy + y, cols[(row >> (6 - 2 * x)) & 3]);
            }
          }
          p += 32; break;
        }
        default:
          p += 32; break;
      }
    }
  }
}

}  // namespace gx
