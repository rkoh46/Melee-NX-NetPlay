// VCDIFF decoding (RFC 3284) with xdelta3's extensions. The DJW secondary decoder below (djw_*) is adapted from
// xdelta3's xdelta3-djw.h and xdelta3-second.h (xdelta 3.1.0, Copyright (C) 2002, 2006, 2007 Joshua P. MacDonald,
// GPL-2.0-or-later, used here under GPL-3.0-or-later): the same bit order, code-length coding (MTF with the 1/2 run
// codes), canonical prefix tables and group selectors, rewritten as plain functions over a byte range.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_vcdiff.h"
#include "mi_hash.h"
#include "mi_lzma.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace modinstall {
namespace {

// ---- DJW (xdelta3's static Huffman secondary compressor) ----
constexpr unsigned kDjwMaxCodelen = 20, kDjwTotalCodes = kDjwMaxCodelen + 2, kDjwBasicCodes = 5, kDjwRunCodes = 2,
                   kDjwExtra12Offset = kDjwBasicCodes + kDjwRunCodes, kDjwExtraCodeBits = 4, kDjwMaxGroups = 8, kDjwGroupBits = 3,
                   kDjwSectorszMult = 5, kDjwSectorszBits = 5, kDjwMaxClclen = 15, kDjwClclenBits = 4, kDjwMaxGbclen = 7,
                   kDjwGbclenBits = 3, kAlphabet = 256, kRun1 = 1;
const uint8_t kDjw12Extra[15] = {9, 10, 3, 11, 2, 12, 13, 1, 14, 15, 16, 17, 18, 19, 20};
const uint8_t kDjw12Basic[5] = {4, 5, 6, 7, 8};

struct Bits {
  const uint8_t* in; const uint8_t* end;
  uint32_t cur_byte = 0, cur_mask = 0x100;
  bool next() {
    if (in == end) return false;
    cur_byte = *in++; cur_mask = 1;
    return true;
  }
  bool get(unsigned nbits, uint32_t* value) {
    uint32_t v = 0, vmask = 1u << nbits;
    if (nbits == 0) { *value = 0; return true; }
    for (;;) {
      if (cur_mask == 0x100 && !next()) return false;
      vmask >>= 1;
      if (cur_byte & cur_mask) v |= vmask;
      cur_mask <<= 1;
      if (vmask == 1) { *value = v; return true; }
    }
  }
};

struct Prefix {
  uint8_t inorder[kAlphabet];
  uint32_t base[kDjwTotalCodes + 2], limit[kDjwTotalCodes + 2];
  uint32_t min_clen = 0, max_clen = 0;
};

void djw_build(unsigned asize, unsigned abs_max, const uint8_t* clen, Prefix* p) {
  uint32_t nr[kDjwTotalCodes + 2] = {}, tmp_base[kDjwTotalCodes + 2] = {};
  for (unsigned i = 0; i < asize; ++i) nr[clen[i]]++;
  unsigned i;
  for (i = 1; i <= abs_max; ++i) if (nr[i]) break;
  const unsigned min_clen = i;
  for (i = abs_max; i != 0; --i) if (nr[i]) break;
  const unsigned max_clen = i;
  tmp_base[min_clen] = 0; p->base[min_clen] = 0; p->limit[min_clen] = nr[min_clen] - 1;
  for (i = min_clen + 1; i <= max_clen; ++i) {
    const uint32_t last_limit = (p->limit[i - 1] + 1) << 1;
    tmp_base[i] = tmp_base[i - 1] + nr[i - 1];
    p->limit[i] = last_limit + nr[i] - 1;
    p->base[i] = last_limit - tmp_base[i];
  }
  for (i = 0; i < asize; ++i) if (clen[i] != 0) p->inorder[tmp_base[clen[i]]++] = uint8_t(i);
  p->min_clen = min_clen; p->max_clen = max_clen;
}

bool djw_symbol(Bits& b, const Prefix& p, uint32_t max_sym, uint32_t* sym) {
  uint32_t code = 0, bits = 0;
  for (;;) {
    if (b.cur_mask == 0x100 && !b.next()) return false;
    do {
      if (bits == p.max_clen) return false;
      bits += 1;
      code <<= 1;
      if (b.cur_byte & b.cur_mask) code |= 1;
      b.cur_mask <<= 1;
      if (bits >= p.min_clen && code <= p.limit[bits]) {
        if (p.base[bits] > code) return false;
        const uint32_t offset = code - p.base[bits];
        if (offset > max_sym) return false;
        *sym = p.inorder[offset];
        return true;
      }
    } while (b.cur_mask != 0x100);
  }
}

uint32_t djw_update_mtf(uint8_t* mtf, uint32_t i) {
  const uint8_t sym = mtf[i];
  for (uint32_t k = i; k != 0; --k) mtf[k] = mtf[k - 1];
  mtf[0] = sym;
  return sym;
}

bool djw_decode_1_2(Bits& b, const Prefix& p, uint8_t* mtfvals, uint32_t elts, uint32_t skip_offset, uint8_t* values) {
  uint32_t n = 0, rep = 0, mtf = 0, s = 0;
  while (n < elts) {
    if (skip_offset != 0 && n >= skip_offset && values[n - skip_offset] == 0) { values[n++] = 0; continue; }
    if (rep != 0) { values[n++] = mtfvals[0]; rep -= 1; continue; }
    if (mtf != 0) { values[n++] = uint8_t(djw_update_mtf(mtfvals, mtf)); mtf = 0; continue; }
    if (!djw_symbol(b, p, kDjwTotalCodes, &mtf)) return false;
    if (mtf <= kRun1) { rep = (mtf + 1) << s; mtf = 0; s += 1; if (s > 30) return false; }
    else { mtf -= 1; s = 0; }
  }
  return rep == 0;
}

bool djw_decode(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
  if (out_size == 0) return false;
  Bits b{in, in + in_size};
  uint32_t groups = 0, sector_size = 0;
  if (!b.get(kDjwGroupBits, &groups)) return false;
  groups += 1;
  if (groups > 1) {
    if (!b.get(kDjwSectorszBits, &sector_size)) return false;
    sector_size = (sector_size + 1) * kDjwSectorszMult;
  } else {
    sector_size = uint32_t(out_size);
  }
  const size_t sectors = 1 + (out_size - 1) / sector_size;
  std::vector<Prefix> tables(groups);
  {
    // The code-length code, then every group's code lengths through it.
    uint32_t num_codes = 0;
    if (!b.get(kDjwExtraCodeBits, &num_codes)) return false;
    num_codes += kDjwExtra12Offset;
    uint8_t cl_clen[kDjwTotalCodes] = {};
    for (uint32_t i = 0; i < num_codes && i < kDjwTotalCodes; ++i) { uint32_t v; if (!b.get(kDjwClclenBits, &v)) return false; cl_clen[i] = uint8_t(v); }
    Prefix cl;
    djw_build(kDjwTotalCodes, kDjwMaxClclen, cl_clen, &cl);
    uint8_t cl_mtf[kDjwTotalCodes] = {};
    unsigned k = 0;
    cl_mtf[k++] = 0;
    for (unsigned i = 0; i < kDjwBasicCodes; ++i) cl_mtf[k++] = kDjw12Basic[i];
    for (unsigned i = 0; i < 15; ++i) cl_mtf[k++] = kDjw12Extra[i];
    std::vector<uint8_t> clen(size_t(kAlphabet) * groups);
    if (!djw_decode_1_2(b, cl, cl_mtf, kAlphabet * groups, kAlphabet, clen.data())) return false;
    for (uint32_t g = 0; g < groups; ++g) {
      for (unsigned i = 0; i < kAlphabet; ++i) if (clen[g * kAlphabet + i] > kDjwMaxCodelen) return false;
      djw_build(kAlphabet, kDjwMaxCodelen, &clen[g * kAlphabet], &tables[g]);
    }
  }
  std::vector<uint8_t> sel_group;
  if (groups > 1) {
    uint8_t sel_clen[kDjwMaxGroups + 2] = {}, sel_mtf[kDjwMaxGroups + 2] = {};
    for (uint32_t g = 0; g < groups + 1; ++g) { uint32_t v; if (!b.get(kDjwGbclenBits, &v)) return false; sel_clen[g] = uint8_t(v); sel_mtf[g] = uint8_t(g); }
    Prefix sel;
    djw_build(groups + 1, kDjwMaxGbclen, sel_clen, &sel);
    sel_group.resize(sectors);
    if (!djw_decode_1_2(b, sel, sel_mtf, uint32_t(sectors), 0, sel_group.data())) return false;
  }
  uint8_t* o = out;
  uint8_t* const end = out + out_size;
  for (size_t c = 0; c < sectors; ++c) {
    const uint32_t g = groups >= 2 ? sel_group[c] : 0;
    if (g >= groups) return false;
    const Prefix& t = tables[g];
    size_t n = std::min<size_t>(sector_size, size_t(end - o));
    while (n--) {
      uint32_t sym;
      if (!djw_symbol(b, t, kAlphabet, &sym)) return false;
      *o++ = uint8_t(sym);
    }
  }
  return true;
}

// ---- VCDIFF ----
enum : uint8_t { NOOP = 0, ADD = 1, RUN = 2, COPY = 3 };
struct Half { uint8_t type = NOOP, size = 0, mode = 0; };
struct CodeEntry { Half a, b; };

const CodeEntry* default_code_table() {
  static CodeEntry table[256];
  static bool built = false;
  if (built) return table;
  int i = 0;
  table[i++] = {{RUN, 0, 0}, {}};
  for (int s = 0; s <= 17; ++s) table[i++] = {{ADD, uint8_t(s), 0}, {}};
  for (int m = 0; m <= 8; ++m) {
    table[i++] = {{COPY, 0, uint8_t(m)}, {}};
    for (int s = 4; s <= 18; ++s) table[i++] = {{COPY, uint8_t(s), uint8_t(m)}, {}};
  }
  for (int m = 0; m <= 5; ++m)
    for (int a = 1; a <= 4; ++a)
      for (int s = 4; s <= 6; ++s) table[i++] = {{ADD, uint8_t(a), 0}, {COPY, uint8_t(s), uint8_t(m)}};
  for (int m = 6; m <= 8; ++m)
    for (int a = 1; a <= 4; ++a) table[i++] = {{ADD, uint8_t(a), 0}, {COPY, 4, uint8_t(m)}};
  for (int m = 0; m <= 8; ++m) table[i++] = {{COPY, 4, uint8_t(m)}, {ADD, 1, 0}};
  built = true;
  return table;
}

// The patch, read through a buffer.
class PatchReader {
 public:
  PatchReader(ByteSource& src) : src_(src), buf_(1u << 16) {}
  bool eof() { return !fill() ; }
  bool byte(uint8_t* out) { if (!fill()) return false; *out = buf_[at_++]; return true; }
  bool bytes(uint8_t* out, size_t n) {
    while (n) {
      if (!fill()) return false;
      const size_t take = std::min(n, have_ - at_);
      std::memcpy(out, &buf_[at_], take);
      at_ += take; out += take; n -= take;
    }
    return true;
  }
  bool integer(uint64_t* out) {
    uint64_t v = 0;
    for (int i = 0; i < 10; ++i) {
      uint8_t b;
      if (!byte(&b)) return false;
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;
  }
  bool failed() const { return failed_; }
 private:
  bool fill() {
    if (at_ < have_) return true;
    if (failed_ || ended_) return false;
    const long got = src_.read(buf_.data(), buf_.size());
    if (got < 0) { failed_ = true; return false; }
    if (got == 0) { ended_ = true; return false; }
    at_ = 0; have_ = size_t(got);
    return true;
  }
  ByteSource& src_;
  std::vector<uint8_t> buf_;
  size_t at_ = 0, have_ = 0;
  bool failed_ = false, ended_ = false;
};

// Reads inside a decoded section.
struct Section {
  const uint8_t* p = nullptr; size_t n = 0, at = 0;
  bool byte(uint8_t* out) { if (at >= n) return false; *out = p[at++]; return true; }
  bool integer(uint64_t* out) {
    uint64_t v = 0;
    for (int i = 0; i < 10; ++i) {
      uint8_t b;
      if (!byte(&b)) return false;
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;
  }
};

struct AddressCache {
  uint64_t near[4] = {}, same[3 * 256] = {};
  unsigned next = 0;
  void reset() { std::memset(near, 0, sizeof near); std::memset(same, 0, sizeof same); next = 0; }
  void update(uint64_t addr) { near[next] = addr; next = (next + 1) % 4; same[addr % (3 * 256)] = addr; }
  bool decode(Section& s, uint64_t here, unsigned mode, uint64_t* addr) {
    uint64_t v = 0;
    if (mode == 0) { if (!s.integer(&v)) return false; *addr = v; }
    else if (mode == 1) { if (!s.integer(&v) || v > here) return false; *addr = here - v; }
    else if (mode < 6) { if (!s.integer(&v)) return false; *addr = near[mode - 2] + v; }
    else { uint8_t b; if (!s.byte(&b)) return false; *addr = same[(mode - 6) * 256 + b]; }
    update(*addr);
    return true;
  }
};

constexpr uint8_t VCD_DECOMPRESS = 0x01, VCD_CODETABLE = 0x02, VCD_APPHEADER = 0x04;
constexpr uint8_t VCD_SOURCE = 0x01, VCD_TARGET = 0x02, VCD_ADLER32 = 0x04;
constexpr uint8_t VCD_DATACOMP = 0x01, VCD_INSTCOMP = 0x02, VCD_ADDRCOMP = 0x04;
constexpr uint64_t kMaxWindow = 64u << 20;

}  // namespace

bool vcdiff_decode(ByteSource& patch, RandomFile& source, OutputSink& out, const std::function<bool(uint64_t, uint64_t)>& progress,
                   std::string* error, VcdiffInfo* info) {
  PatchReader in(patch);
  auto fail = [&](const std::string& why) {
    *error = in.failed() ? (patch.error().empty() ? why : patch.error()) : why;
    return false;
  };
  uint8_t head[5];
  if (!in.bytes(head, 5)) return fail("the patch is too short");
  if (head[0] != 0xD6 || head[1] != 0xC3 || head[2] != 0xC4 || head[3] != 0) return fail("not a VCDIFF patch (xdelta3)");
  const uint8_t hdr = head[4];
  if (hdr & ~(VCD_DECOMPRESS | VCD_CODETABLE | VCD_APPHEADER)) return fail("unknown VCDIFF header bits");
  if (hdr & VCD_CODETABLE) return fail("custom VCDIFF code tables are not supported");
  int secondary = 0;
  if (hdr & VCD_DECOMPRESS) {
    uint8_t id;
    if (!in.byte(&id)) return fail("the patch is too short");
    if (id != 1 && id != 2) return fail("the patch's secondary compressor (" + std::to_string(id) + ") is not supported");
    secondary = id;
  }
  if (hdr & VCD_APPHEADER) {
    uint64_t len;
    if (!in.integer(&len) || len > 65536) return fail("invalid application header");
    std::vector<uint8_t> app(static_cast<size_t>(len));
    if (!in.bytes(app.data(), app.size())) return fail("the patch is too short");
  }
  if (info) info->secondary = secondary;
  const CodeEntry* table = default_code_table();
  std::unique_ptr<XzRunOn> xz[3];
  if (secondary == 2) for (auto& x : xz) x.reset(new XzRunOn);
  std::vector<uint8_t> target, raw[3], plain[3];
  AddressCache cache;
  uint64_t windows = 0, written = 0;
  for (;;) {
    uint8_t win;
    if (!in.byte(&win)) {
      if (in.failed()) return fail("cannot read the patch");
      break;   // the end of the patch
    }
    if (progress && !progress(patch.consumed(), patch.total())) { *error = "cancelled"; return false; }
    if (win & ~(VCD_SOURCE | VCD_TARGET | VCD_ADLER32)) return fail("unknown window bits");
    if (win & VCD_TARGET) return fail("target-window copies (VCD_TARGET) are not supported");
    uint64_t cpylen = 0, cpyoff = 0, enclen, tgtlen, datalen, instlen, addrlen;
    uint8_t delind;
    if (win & VCD_SOURCE) {
      if (!in.integer(&cpylen) || !in.integer(&cpyoff)) return fail("the patch is damaged (window)");
      if (cpyoff > source.size() || cpylen > source.size() - cpyoff) return fail("the patch reads past the end of the source disc: is it the vanilla NTSC 1.02 disc?");
    }
    if (!in.integer(&enclen) || !in.integer(&tgtlen) || !in.byte(&delind) || !in.integer(&datalen) || !in.integer(&instlen) || !in.integer(&addrlen))
      return fail("the patch is damaged (window header)");
    if (tgtlen > kMaxWindow || datalen > kMaxWindow || instlen > kMaxWindow || addrlen > kMaxWindow) return fail("a patch window is too large");
    uint32_t adler = 0;
    if (win & VCD_ADLER32) {
      uint8_t a[4];
      if (!in.bytes(a, 4)) return fail("the patch is damaged (checksum)");
      adler = uint32_t(a[0]) << 24 | uint32_t(a[1]) << 16 | uint32_t(a[2]) << 8 | a[3];
    }
    if ((delind & ~(VCD_DATACOMP | VCD_INSTCOMP | VCD_ADDRCOMP)) || (delind && !secondary)) return fail("invalid delta indicator");
    const uint64_t lens[3] = {datalen, instlen, addrlen};
    const uint8_t bits[3] = {VCD_DATACOMP, VCD_INSTCOMP, VCD_ADDRCOMP};
    Section sec[3];
    for (int k = 0; k < 3; ++k) {
      raw[k].resize(size_t(lens[k]));
      if (lens[k] && !in.bytes(raw[k].data(), raw[k].size())) return fail("the patch ends inside a window");
      if (delind & bits[k]) {
        // A secondary-compressed section: its decoded size, then the compressed bytes.
        Section s{raw[k].data(), raw[k].size()};
        uint64_t size;
        if (!s.integer(&size) || size == 0 || size > kMaxWindow) return fail("invalid secondary section size");
        plain[k].resize(size_t(size));
        const bool ok = secondary == 1 ? djw_decode(raw[k].data() + s.at, raw[k].size() - s.at, plain[k].data(), plain[k].size())
                                       : xz[k]->decode(raw[k].data() + s.at, raw[k].size() - s.at, plain[k].data(), plain[k].size());
        if (!ok) return fail(secondary == 1 ? "the patch is damaged (DJW section)" : "the patch is damaged (LZMA section)");
        sec[k] = {plain[k].data(), plain[k].size()};
      } else {
        sec[k] = {raw[k].data(), raw[k].size()};
      }
    }
    target.resize(size_t(tgtlen));
    cache.reset();
    uint64_t pos = 0;
    Section& data = sec[0];
    Section& inst = sec[1];
    Section& addr = sec[2];
    while (inst.at < inst.n) {
      uint8_t index;
      inst.byte(&index);
      const CodeEntry& e = table[index];
      for (const Half* h : {&e.a, &e.b}) {
        if (h->type == NOOP) continue;
        uint64_t size = h->size;
        if (size == 0 && !inst.integer(&size)) return fail("the patch is damaged (instruction size)");
        if (size > tgtlen - pos) return fail("the patch is damaged (instruction past the window)");
        uint8_t* dst = target.data() + pos;
        if (h->type == ADD) {
          if (size > data.n - data.at) return fail("the patch is damaged (data section)");
          std::memcpy(dst, data.p + data.at, size_t(size));
          data.at += size_t(size);
        } else if (h->type == RUN) {
          uint8_t b;
          if (!data.byte(&b)) return fail("the patch is damaged (data section)");
          std::memset(dst, b, size_t(size));
        } else {
          uint64_t a;
          if (!cache.decode(addr, cpylen + pos, h->mode, &a)) return fail("the patch is damaged (address section)");
          if (a < cpylen) {
            if (size > cpylen - a) return fail("the patch is damaged (a copy crosses the source window)");
            if (!source.read(cpyoff + a, dst, size_t(size))) return fail("cannot read the source disc");
          } else {
            const uint64_t from = a - cpylen;
            if (from >= pos) return fail("the patch is damaged (a copy from ahead)");
            const uint8_t* src = target.data() + from;
            for (uint64_t i = 0; i < size; ++i) dst[i] = src[i];   // may overlap: byte by byte, as the RFC says
          }
        }
        pos += size;
      }
    }
    if (pos != tgtlen) return fail("the patch is damaged (wrong window length)");
    if (data.at != data.n || addr.at != addr.n) return fail("the patch is damaged (extra section bytes)");
    if ((win & VCD_ADLER32) && adler32_update(1, target.data(), target.size()) != adler) return fail("a patch window's checksum does not match: is the source the vanilla NTSC 1.02 disc?");
    if (!out.write(target.data(), target.size())) { *error = "cannot write the new disc (is the SD card full?)"; return false; }
    written += tgtlen;
    ++windows;
  }
  if (windows == 0) return fail("the patch has no windows");
  if (info) { info->windows = windows; info->target_bytes = written; }
  if (progress) progress(patch.consumed(), patch.total());
  return true;
}

}  // namespace modinstall
