// SPDX-License-Identifier: GPL-2.0-or-later
#include "vcdiff.h"
#include <cstring>

namespace host {
namespace {

struct Reader {
  const uint8_t* p; const uint8_t* end;
  bool ok = true;
  uint8_t byte() { if (p >= end) { ok = false; return 0; } return *p++; }
  // RFC 3284 base-128 integer, most significant group first.
  uint32_t varint() {
    uint32_t v = 0;
    for (int i = 0; i < 5; ++i) {
      uint8_t b = byte();
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) return v;
    }
    ok = false; return 0;
  }
  bool has(size_t n) const { return (size_t)(end - p) >= n; }
};

enum { VCD_NOOP = 0, VCD_ADD = 1, VCD_RUN = 2, VCD_COPY = 3 };
struct Entry { uint8_t t1, s1, m1, t2, s2, m2; };

// The default code table from RFC 3284 section 5.6.
const Entry* default_table() {
  static Entry table[256];
  static bool built = false;
  if (built) return table;
  int i = 0;
  table[i++] = {VCD_RUN, 0, 0, VCD_NOOP, 0, 0};
  for (int s = 0; s <= 17; ++s) table[i++] = {VCD_ADD, (uint8_t)s, 0, VCD_NOOP, 0, 0};
  for (int m = 0; m <= 8; ++m) {
    table[i++] = {VCD_COPY, 0, (uint8_t)m, VCD_NOOP, 0, 0};
    for (int s = 4; s <= 18; ++s) table[i++] = {VCD_COPY, (uint8_t)s, (uint8_t)m, VCD_NOOP, 0, 0};
  }
  for (int m = 0; m <= 5; ++m)
    for (int a = 1; a <= 4; ++a)
      for (int s = 4; s <= 6; ++s) table[i++] = {VCD_ADD, (uint8_t)a, 0, VCD_COPY, (uint8_t)s, (uint8_t)m};
  for (int m = 6; m <= 8; ++m)
    for (int a = 1; a <= 4; ++a) table[i++] = {VCD_ADD, (uint8_t)a, 0, VCD_COPY, 4, (uint8_t)m};
  for (int m = 0; m <= 8; ++m) table[i++] = {VCD_COPY, 4, (uint8_t)m, VCD_ADD, 1, 0};
  built = true;
  return table;
}

struct AddressCache {
  static constexpr int NEAR = 4, SAME = 3;
  uint32_t near_[NEAR] = {}, same_[SAME * 256] = {};
  int next_near = 0;
  void reset() { std::memset(near_, 0, sizeof near_); std::memset(same_, 0, sizeof same_); next_near = 0; }
  void update(uint32_t addr) {
    near_[next_near] = addr; next_near = (next_near + 1) % NEAR;
    same_[addr % (SAME * 256)] = addr;
  }
  bool decode(Reader& addrs, uint32_t here, int mode, uint32_t& out) {
    if (mode == 0) out = addrs.varint();
    else if (mode == 1) out = here - addrs.varint();
    else if (mode < 2 + NEAR) out = near_[mode - 2] + addrs.varint();
    else if (mode < 2 + NEAR + SAME) out = same_[(mode - 2 - NEAR) * 256 + addrs.byte()];
    else return false;
    if (!addrs.ok) return false;
    update(out);
    return true;
  }
};

bool fail(std::string* error, const char* what) { if (error) *error = what; return false; }

}  // namespace

bool vcdiff_decode(const uint8_t* source, size_t source_size, const uint8_t* delta, size_t delta_size,
                   std::vector<uint8_t>& target, std::string* error) {
  Reader r{delta, delta + delta_size};
  if (delta_size < 5 || delta[0] != 0xD6 || delta[1] != 0xC3 || delta[2] != 0xC4 || delta[3] != 0x00) return fail(error, "not a VCDIFF stream");
  r.p += 4;
  uint8_t hdr = r.byte();
  if (hdr & 0x01) return fail(error, "secondary compression not supported");
  if (hdr & 0x02) return fail(error, "custom code table not supported");
  if (hdr & 0x04) { uint32_t n = r.varint(); if (!r.has(n)) return fail(error, "truncated app header"); r.p += n; }
  const Entry* table = default_table();
  target.clear();
  AddressCache cache;
  while (r.p < r.end) {
    uint8_t win = r.byte();
    const uint8_t* seg = nullptr; uint32_t seg_len = 0;
    if (win & 0x03) {
      seg_len = r.varint();
      uint32_t seg_pos = r.varint();
      if (win & 0x01) { if ((uint64_t)seg_pos + seg_len > source_size) return fail(error, "source segment out of range"); seg = source + seg_pos; }
      else { if ((uint64_t)seg_pos + seg_len > target.size()) return fail(error, "target segment out of range"); seg = nullptr; /* resolved below */ }
    }
    uint32_t delta_len = r.varint(); (void)delta_len;
    uint32_t target_len = r.varint();
    uint8_t delta_ind = r.byte();
    if (delta_ind & 0x07) return fail(error, "compressed sections not supported");
    uint32_t data_len = r.varint(), inst_len = r.varint(), addr_len = r.varint();
    if (win & 0x04) r.varint();   // open-vcdiff checksum extension
    if (!r.ok || !r.has((size_t)data_len + inst_len + addr_len)) return fail(error, "truncated window");
    Reader data{r.p, r.p + data_len};
    Reader inst{r.p + data_len, r.p + data_len + inst_len};
    Reader addrs{r.p + data_len + inst_len, r.p + data_len + inst_len + addr_len};
    r.p += data_len + inst_len + addr_len;
    // Window working buffer: [source segment][new target bytes]; copies may reference both.
    std::vector<uint8_t> window;
    window.reserve(seg_len + target_len);
    if (seg_len) {
      if (win & 0x01) window.insert(window.end(), seg, seg + seg_len);
      else { size_t pos = target.size() - 0; (void)pos; /* target-segment windows are unused by Slippi diffs */ return fail(error, "target-segment windows not supported"); }
    }
    cache.reset();
    while (inst.p < inst.end) {
      const Entry& e = table[inst.byte()];
      for (int half = 0; half < 2; ++half) {
        uint8_t type = half ? e.t2 : e.t1, size8 = half ? e.s2 : e.s1, mode = half ? e.m2 : e.m1;
        if (type == VCD_NOOP) continue;
        uint32_t size = size8 ? size8 : inst.varint();
        if (!inst.ok) return fail(error, "bad instruction stream");
        if (type == VCD_ADD) {
          if (!data.has(size)) return fail(error, "ADD past data section");
          window.insert(window.end(), data.p, data.p + size); data.p += size;
        } else if (type == VCD_RUN) {
          uint8_t b = data.byte();
          window.insert(window.end(), size, b);
        } else {
          uint32_t addr;
          uint32_t here = (uint32_t)window.size();
          if (!cache.decode(addrs, here, mode, addr)) return fail(error, "bad COPY address");
          if (addr >= here) return fail(error, "COPY address out of range");
          for (uint32_t k = 0; k < size; ++k) window.push_back(window[addr + k]);   // overlap-safe
        }
      }
    }
    if (window.size() < seg_len || window.size() - seg_len != target_len) return fail(error, "window length mismatch");
    target.insert(target.end(), window.begin() + seg_len, window.end());
  }
  return r.ok;
}

}  // namespace host
