// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include "gx_aurora_bridge.h"
#include <memory>
#include <cstdio>
#include <cstring>
#include <set>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace gx::aurora_bridge {
namespace {
using Bytes = std::vector<uint8_t>;
inline void u16(Bytes& out, uint16_t value) {
  out.push_back(uint8_t(value >> 8)); out.push_back(uint8_t(value));
}
// One big-endian word appended with a single size change: four push_backs here were a tenth of the
// render thread (every register, matrix element and vertex component passes through this).
inline void u32(Bytes& out, uint32_t value) {
  const size_t at = out.size();
  out.resize(at + 4);
  const uint32_t be = __builtin_bswap32(value);
  std::memcpy(out.data() + at, &be, 4);
}
void f32(Bytes& out, float value) { u32(out, std::bit_cast<uint32_t>(value)); }
void bp(Bytes& out, uint8_t reg, uint32_t value) {
  out.push_back(0x61); u32(out, uint32_t(reg) << 24 | (value & 0xffffff));
}
void cp(Bytes& out, uint8_t reg, uint32_t value) {
  out.push_back(0x08); out.push_back(reg); u32(out, value);
}
void xf_header(Bytes& out, uint16_t addr, uint16_t words) {
  out.push_back(0x10); u16(out, uint16_t(words - 1)); u16(out, addr);
}
void xf_words(Bytes& out, uint16_t addr, const uint32_t* words, uint16_t count) {
  xf_header(out, addr, count);
  for (unsigned i = 0; i < count; ++i) u32(out, words[i]);
}
void xf_floats(Bytes& out, uint16_t addr, const float* words, uint16_t count) {
  xf_header(out, addr, count);
  for (unsigned i = 0; i < count; ++i) f32(out, words[i]);
}
[[noreturn]] void fail(const Frame& frame, const std::string& where, const char* reason) {
  throw CoverageError("Aurora coverage: frame " + std::to_string(frame.sequence) + " " + where + ": " + reason);
}
// Unsupported-but-harmless render state: reported once per reason, then rendered as-is.
void warn(const Frame& frame, const std::string& where, const char* reason) {
  static std::set<std::string> reported;
  if (reported.insert(reason).second)
    std::fprintf(stderr, "aurora bridge: frame %llu %s: %s (rendered without it; reported once)\n",
                 (unsigned long long)frame.sequence, where.c_str(), reason);
}
bool texture_format(uint32_t f) { return f <= 6 || f == 8 || f == 9 || f == 10 || f == 14; }
bool matrix_row(uint32_t row, bool identity) { return (row <= 57 && row % 3 == 0) || (identity && row == 60); }

void validate_texture(const Frame& frame, size_t index, unsigned unit, const TextureRef& t) {
  // The location text is built only when a check fails (it was two string allocations per texture per draw).
  const auto where = [&] { return "draw " + std::to_string(index) + " texture " + std::to_string(unit); };
  if (!t.data || !t.width || !t.height || t.width > 1024 || t.height > 1024 || !texture_format(t.format))
    fail(frame, where(), "invalid texture shape, format, or missing immutable bytes");
  const auto filter = bits(t.mode0, 5, 3);
  if (bits(t.mode0, 0, 2) > 2 || bits(t.mode0, 2, 2) > 2 || filter == 3 || filter == 7 || bits(t.mode0, 19, 2) > 2)
    fail(frame, where(), "unsupported texture wrap/filter/anisotropy encoding");
  const bool mips = (filter & 3) != 0;
  const auto max_lod = bits(t.mode1, 8, 8);
  // Aurora infers uploaded mip count from floor(maxLOD); partial mip LOD needs
  // separate upload-level metadata before it can be represented without drift.
  if (mips && (max_lod % 16 != 0 || max_lod / 16 + 1 != t.mip_levels))
    fail(frame, where(), "fractional or clamped mip-count mapping is not implemented");
  if (t.mip_levels != texture_mip_count(t.width, t.height, t.mip_levels) || (!mips && t.mip_levels != 1) ||
      t.data->image.size() < texture_chain_bytes(t.width, t.height, t.format, t.mip_levels))
    fail(frame, where(), "texture mip chain is incomplete");
  const uint32_t palette_bytes = t.format == 8 ? 32 : t.format == 9 ? 512 : t.format == 10 ? 32768 : 0;
  if (palette_bytes && (t.tlut_format > 2 || t.data->palette.size() < palette_bytes))
    fail(frame, where(), "palette format or immutable palette bytes are incomplete");
}

// Which DrawState parts may differ from the last emitted draw's: the union of the deltas recorded
// since then (apply_state's records). Matrix blocks, lights and XF ranges outside it are equal by
// construction, so convert_draw compares only the marked ones and prepare_frame copies only those
// into `emitted`; a 4-player frame compared and copied ~2.5 KB and 4.4 KB per draw, 1400 draws.
struct Dirty {
  bool all = true;
  uint64_t bp[4] = {}, pos = 0, post = 0, xf[2] = {};
  uint32_t nrm = 0;
  uint8_t lights = 0;
  bool tev = false;
  void add(const StateDelta& d) {
    if (d.full) { all = true; return; }
    for (unsigned i = 0; i < 4; ++i) bp[i] |= d.bp[i];
    pos |= d.pos; post |= d.post; xf[0] |= d.xf[0]; xf[1] |= d.xf[1]; nrm |= d.nrm; lights |= d.lights; tev |= d.tev;
  }
  void reset() { *this = Dirty{}; all = false; }
  bool rows(uint64_t m, unsigned first, unsigned n) const { return all || ((m >> first) & ((1ull << n) - 1)) != 0; }
  bool xf_range(unsigned lo, unsigned n) const {
    if (all) return true;
    for (unsigned r = lo; r < lo + n; ++r) if ((xf[r >> 6] >> (r & 63)) & 1) return true;
    return false;
  }
  // Brings `to` up to date with `from` in the marked parts (everything when `all`).
  void copy(DrawState& to, const DrawState& from) const {
    if (all) { to = from; return; }
    for (unsigned i = 0; i < 4; ++i) for (uint64_t m = bp[i]; m; m &= m - 1) { const unsigned r = i * 64 + __builtin_ctzll(m); to.bp.reg[r] = from.bp.reg[r]; }
    for (uint64_t m = pos; m; m &= m - 1) std::memcpy(to.posMatrices + __builtin_ctzll(m) * 4, from.posMatrices + __builtin_ctzll(m) * 4, 16);
    for (uint32_t m = nrm; m; m &= m - 1) std::memcpy(to.normalMatrices + __builtin_ctz(m) * 3, from.normalMatrices + __builtin_ctz(m) * 3, 12);
    for (uint64_t m = post; m; m &= m - 1) std::memcpy(to.postMatrices + __builtin_ctzll(m) * 4, from.postMatrices + __builtin_ctzll(m) * 4, 16);
    for (unsigned m = lights; m; m &= m - 1) std::memcpy(to.lights[__builtin_ctz(m)], from.lights[__builtin_ctz(m)], 64);
    for (unsigned i = 0; i < 2; ++i) for (uint64_t m = xf[i]; m; m &= m - 1) { const unsigned r = i * 64 + __builtin_ctzll(m); to.xf_regs[r] = from.xf_regs[r]; }
    to.matrix_index_a = from.matrix_index_a; to.matrix_index_b = from.matrix_index_b;
    if (tev) { std::memcpy(to.tev_colors, from.tev_colors, sizeof to.tev_colors); std::memcpy(to.tev_kcolors, from.tev_kcolors, sizeof to.tev_kcolors); }
  }
};

void convert_draw(const Frame& frame, size_t index, Extent extent, const DrawState& st, const DrawCall* prev, const DrawState* pst, const Dirty& dirty, Draw& out) {
  // `d` holds the draw's own data (vertices, textures, formats); `st` the register and matrix state it
  // renders with, rebuilt from the frame's deltas, and `pst` that of the previous emitted draw.
  const auto& d = frame.draws[index];
  const auto where = [&] { return "draw " + std::to_string(index); };   // built only on failure
  const auto check = [&](bool ok, const char* reason) { if (!ok) fail(frame, where(), reason); };
  const auto soft = [&](bool ok, const char* reason) { if (!ok) warn(frame, where(), reason); };
  check(d.vertex_count > 0 && d.vertex_count <= 65535 &&
        (d.raw_size || (d.first_vertex <= frame.vertices.size() && d.vertex_count <= frame.vertices.size() - d.first_vertex)),
        "invalid vertex range");
  check(d.primitive == 0x80 || d.primitive == 0x90 || d.primitive == 0x98 || d.primitive == 0xa0,
        "only quads and triangle primitives are supported by the locked baseline");
  check(d.vertex_count >= (d.primitive == 0x80 ? 4u : 3u) &&
        (d.primitive != 0x80 || d.vertex_count % 4 == 0) &&
        (d.primitive != 0x90 || d.vertex_count % 3 == 0), "incomplete primitive");
  constexpr uint32_t supported = 0x1ff | VB_HAS_NRM0 | VB_HAS_NRM1 | VB_HAS_NRM2 | VB_HAS_COL0 | VB_HAS_COL1 |
                                 (0xffu << 15) | VB_UNCAPTURED_NBT;
  check((d.components & ~supported) == 0, "unknown/lost vertex components are unsupported");
  constexpr uint32_t any_normal = VB_HAS_NRM0 | VB_HAS_NRM1 | VB_HAS_NRM2 | VB_UNCAPTURED_NBT;
  soft(d.raw_size || !(d.components & (VB_HAS_NRM1 | VB_HAS_NRM2 | VB_UNCAPTURED_NBT)),
       "NBT binormal/tangent are not captured; the normal alone is used (emboss texgens will be flat)");
  const auto texgens = st.xf_regs[0x3f];
  // Emboss texgens and binormal/tangent sources need nine-component normals in
  // Aurora. The capture carries only the normal, which is replicated: shading
  // from those vectors is approximate, but the textures they address render.
  bool nbt = (d.components & VB_UNCAPTURED_NBT) != 0;
  for (unsigned i = 0; i < texgens && i < 8; ++i) {
    const auto info = st.xf_regs[0x40 + i];
    if (tmi_texgentype(info) == 1) nbt = true;
  }
  if (nbt && !(d.components & (VB_HAS_NRM0 | VB_HAS_NRM1 | VB_HAS_NRM2 | VB_UNCAPTURED_NBT))) nbt = false;
  check(texgens <= 8 && st.xf_regs[0x09] <= 2 && st.bp.numindstages() <= 4, "invalid GX stage/channel count");
  check(texgens == st.bp.numtexgens() && st.xf_regs[0x09] == st.bp.numcolchans(), "BP/XF stage counts disagree");
  soft(!st.bp.zfreeze() && bits(st.bp.ztex2(), 2, 2) == 0, "z-freeze or z-texture is not implemented in Aurora");
  check((st.bp.zcontrol() & 7) <= 1, "only RGB8/RGBA6 EFB pixel formats are covered");
  soft(!(st.bp.blendmode() & 4), "EFB dithering is not implemented in Aurora");
  if ((st.bp.blendmode() & 3) == 2 && !(st.bp.blendmode() & (1u << 11))) {
    const auto op = bits(st.bp.blendmode(), 12, 4);
    soft(op == 0 || op == 3 || op == 5, "logic operation is not implemented in Aurora");
  }
  check(st.xf_regs[0x26] <= 1, "invalid projection kind");
  check((st.matrix_index_a & 63) <= 27 && (st.matrix_index_a & 63) % 3 == 0,
        "default position matrix is outside Aurora's ten aligned matrices");
  for (unsigned i = 0; i < 8; ++i) {
    const uint32_t row = i < 4 ? bits(st.matrix_index_a, 6 + i * 6, 6) : bits(st.matrix_index_b, (i - 4) * 6, 6);
    soft(matrix_row(row, true), "unaligned or out-of-range texture matrix selector");
    if (i < texgens) {
      const auto info = st.xf_regs[0x40 + i];
      // Aurora's shader generator is fatal on texgen sources it does not handle and
      // emboss maps need binormal/tangent vectors the capture does not carry, so
      // such draws are skipped rather than warned about.
      check(tmi_texgentype(info) <= 3, "unknown texgen type");
      soft(tmi_sourcerow(info) < 13, "texgen source row is out of range; position is used instead");
      // Aurora always applies post transforms: disabled dual-transform is
      // explicitly normalized to the documented identity post-matrix below.
      const auto post = st.xf_regs[0x50 + i] & 63;
      soft(!(st.xf_regs[0x12] & 1) || post == 61 || (post <= 57 && post % 3 == 0),
            "unaligned or out-of-range post-texture matrix");
      soft(!(st.bp.texcoord_s(i) & (1u << 17)) && !(st.bp.texcoord_t(i) & (1u << 17)),
            "cylindrical texture-coordinate wrap is not implemented in Aurora");
    }
    if (d.textures[i].used) {
      validate_texture(frame, index, i, d.textures[i]);
      const auto& t = d.textures[i];
      check(t.width == (st.bp.teximage0(i) & 1023) + 1 && t.height == bits(st.bp.teximage0(i), 10, 10) + 1 &&
            t.format == bits(st.bp.teximage0(i), 20, 4) && t.mode0 == st.bp.texmode0(i) && t.mode1 == st.bp.texmode1(i),
            "texture descriptor disagrees with BP snapshot");
    }
  }
  for (unsigned s = 0; s <= st.bp.numtevstages(); ++s) {
    if (st.bp.order_enable(s)) {
      check(unsigned(st.bp.order_texcoord(s)) < texgens, "TEV references a missing texgen");
      check(d.textures[st.bp.order_texmap(s)].used, "TEV texture was not captured");
    }
    const auto ind = st.bp.tevind(s);
    const auto mtx = bits(ind, 9, 4);
    soft(mtx == 0 || (mtx >= 1 && mtx <= 3) || (mtx >= 5 && mtx <= 7) || (mtx >= 9 && mtx <= 11),
          "invalid indirect matrix selector");
    soft(bits(ind, 13, 3) <= 6 && bits(ind, 16, 3) <= 6, "invalid indirect wrapping mode");
  }
  for (unsigned s = 0; s < st.bp.numindstages(); ++s)
    check(d.textures[bits(st.bp.iref(), s * 6, 3)].used && bits(st.bp.iref(), s * 6 + 3, 3) < texgens,
          "indirect stage source was not captured");

  out.state.clear(); out.vertices.clear();   // the caller's Draw keeps its buffers between frames
  out.arrays.fill(nullptr);
  out.primitive = uint8_t(d.primitive); out.vertex_count = uint16_t(d.vertex_count);
  for (unsigned i = 0; i < 8; ++i) out.textures[i] = &d.textures[i];
  auto& state = out.state;
  state.reserve(prev ? 512 : 4096);
  // Aurora's FIFO state persists between draws of one frame, so only registers
  // and matrix blocks that changed since the previous emitted draw are written.
  // Texture mode registers are always written because texture loads between
  // draws go through the SDK and may touch them.
  const auto bp_changed = [&](unsigned r) { return !prev || pst->bp.reg[r] != st.bp.reg[r]; };
  const auto bp_delta = [&](unsigned r) { if (bp_changed(r)) bp(state, uint8_t(r), st.bp.reg[r]); };
  const auto block_changed = [&](const void* a, const void* b, size_t bytes) { return !prev || std::memcmp(a, b, bytes) != 0; };
  // A snapshot has already applied the guest BP mask. Do not replay masks,
  // interrupts, TLUT DMA, or copy triggers as draw state.
  bp(state, BP_BP_MASK, 0xffffff);
  bp_delta(BP_GENMODE);
  for (unsigned r = 0x06; r <= 0x0e; ++r) bp_delta(r);
  for (unsigned r = 0x10; r <= 0x1f; ++r) bp_delta(r);
  // Bake guest scissor offsets because Aurora's BP decoder does not handle 0x59.
  const auto so = st.bp.reg[BP_SCISSOROFFSET];
  const int left = std::clamp(int(bits(st.bp.reg[BP_SCISSORTL], 12, 11)) - int(bits(so, 0, 10) * 2), 0, int(extent.width));
  const int top = std::clamp(int(bits(st.bp.reg[BP_SCISSORTL], 0, 11)) - int(bits(so, 10, 10) * 2), 0, int(extent.height));
  const int right = std::clamp(int(bits(st.bp.reg[BP_SCISSORBR], 12, 11)) - int(bits(so, 0, 10) * 2) + 1, left, int(extent.width));
  const int bottom = std::clamp(int(bits(st.bp.reg[BP_SCISSORBR], 0, 11)) - int(bits(so, 10, 10) * 2) + 1, top, int(extent.height));
  if (bp_changed(BP_SCISSORTL) || bp_changed(BP_SCISSORBR) || bp_changed(BP_SCISSOROFFSET)) {
    bp(state, BP_SCISSORTL, uint32_t(left + 342) << 12 | uint32_t(top + 342));
    bp(state, BP_SCISSORBR, uint32_t(right + 341) << 12 | uint32_t(bottom + 341));
  }
  bp_delta(BP_LINEPTWIDTH);
  for (unsigned r = 0x25; r <= 0x43; ++r) bp_delta(r);
  // GXLoadTexObj supplies pointer metadata and image dimensions first. Do not
  // overwrite IMAGE0 afterward: Aurora resets wide dimensions when it sees it.
  for (unsigned i = 0; i < 8; ++i) if (d.textures[i].used) {
    const unsigned base = i < 4 ? 0x80 : 0xa0;
    bp(state, uint8_t(base + (i & 3)), d.textures[i].mode0);
    bp(state, uint8_t(base + 4 + (i & 3)), d.textures[i].mode1);
  }
  for (unsigned r = 0xc0; r <= 0xdf; ++r) bp_delta(r);
  // BP E0-E7 is a multiplexed port, not a single register bank.
  for (unsigned bank = 0; bank < 2; ++bank) for (unsigned i = 0; i < 4; ++i) {
    const auto* color = bank ? st.tev_kcolors[i] : st.tev_colors[i];
    for (unsigned c = 0; c < 4; ++c)
      check(color[c] >= (bank ? 0 : -1024) && color[c] <= (bank ? 255 : 1023), "invalid TEV constant component");
    const auto* previous = prev ? (bank ? pst->tev_kcolors[i] : pst->tev_colors[i]) : nullptr;
    if (previous && std::memcmp(previous, color, sizeof(int32_t) * 4) == 0) continue;
    const auto type = bank << 23;
    bp(state, uint8_t(0xe0 + i * 2), type | (uint32_t(color[0]) & 0x7ff) | ((uint32_t(color[3]) & 0x7ff) << 12));
    bp(state, uint8_t(0xe1 + i * 2), type | (uint32_t(color[2]) & 0x7ff) | ((uint32_t(color[1]) & 0x7ff) << 12));
  }
  for (unsigned r = 0xe8; r <= 0xf3; ++r) bp_delta(r);
  for (unsigned r = 0xf6; r <= 0xfd; ++r) bp_delta(r);

  // Aurora accepts complete SDK-aligned matrix blocks only, not the generic
  // hardware XF RAM image. Each float is encoded, never structure-copied.
  for (unsigned i = 0; i < 20; ++i)
    if (dirty.rows(dirty.pos, i * 3, 3) && block_changed(st.posMatrices + i * 12, prev ? pst->posMatrices + i * 12 : nullptr, 12 * sizeof(float)))
      xf_floats(state, uint16_t(i * 12), st.posMatrices + i * 12, 12);
  for (unsigned i = 0; i < 10; ++i)
    if (dirty.rows(dirty.nrm, i * 3, 3) && block_changed(st.normalMatrices + i * 9, prev ? pst->normalMatrices + i * 9 : nullptr, 9 * sizeof(float)))
      xf_floats(state, uint16_t(0x400 + i * 9), st.normalMatrices + i * 9, 9);
  for (unsigned i = 0; i < 20; ++i)
    if (dirty.rows(dirty.post, i * 3, 3) && block_changed(st.postMatrices + i * 12, prev ? pst->postMatrices + i * 12 : nullptr, 12 * sizeof(float)))
      xf_floats(state, uint16_t(0x500 + i * 12), st.postMatrices + i * 12, 12);
  for (unsigned i = 0; i < 8; ++i) {
    if (!dirty.rows(dirty.lights, i, 1) || !block_changed(st.lights[i], prev ? pst->lights[i] : nullptr, 64)) continue;
    xf_header(state, uint16_t(0x600 + i * 16), 16);
    for (unsigned w = 0; w < 16; ++w) {
      // lights[] is a byte view of host-endian XF words, including its color.
      const std::array<uint8_t, 4> bytes{st.lights[i][w * 4], st.lights[i][w * 4 + 1], st.lights[i][w * 4 + 2], st.lights[i][w * 4 + 3]};
      u32(state, std::bit_cast<uint32_t>(bytes));
    }
  }
  if (dirty.xf_range(0x09, 9) && block_changed(st.xf_regs + 0x09, prev ? pst->xf_regs + 0x09 : nullptr, 9 * 4)) xf_words(state, 0x1009, st.xf_regs + 0x09, 9);
  const uint32_t indices[2]{st.matrix_index_a, st.matrix_index_b};
  const bool indices_changed = !prev || pst->matrix_index_a != st.matrix_index_a || pst->matrix_index_b != st.matrix_index_b;
  if (indices_changed) xf_words(state, 0x1018, indices, 2);
  std::array<float, 6> viewport;
  for (unsigned i = 0; i < 6; ++i) {
    viewport[i] = std::bit_cast<float>(st.xf_regs[0x1a + i]);
    check(std::isfinite(viewport[i]), "non-finite viewport");
  }
  check(viewport[0] > 0 && viewport[1] < 0 && viewport[2] >= 0 && viewport[5] >= viewport[2] && viewport[5] <= 16777215.f,
        "flipped, empty or out-of-range viewport is not covered");
  viewport[3] -= 2; viewport[4] -= 2; // Aurora decoder subtracts 340; hardware uses 342.
  if (dirty.xf_range(0x1a, 6) && block_changed(st.xf_regs + 0x1a, prev ? pst->xf_regs + 0x1a : nullptr, 6 * 4)) xf_floats(state, 0x101a, viewport.data(), 6);
  if (dirty.xf_range(0x20, 7) && block_changed(st.xf_regs + 0x20, prev ? pst->xf_regs + 0x20 : nullptr, 7 * 4)) xf_words(state, 0x1020, st.xf_regs + 0x20, 7);
  if (dirty.xf_range(0x3f, 9) && block_changed(st.xf_regs + 0x3f, prev ? pst->xf_regs + 0x3f : nullptr, 9 * 4)) {
    // Aurora leaves a texgen's source unset when the XF row is outside its table and
    // then aborts shader generation if any TEV stage samples it. Rows it cannot
    // serve (binormal/tangent, out of range) are rewritten to the position row.
    uint32_t texgen[9];
    texgen[0] = st.xf_regs[0x3f];
    for (unsigned i = 0; i < 8; ++i) {
      uint32_t info = st.xf_regs[0x40 + i];
      const auto type = tmi_texgentype(info), row = tmi_sourcerow(info);
      if (type != 2 && type != 3 && (row >= 13 || row == 3 || row == 4)) info &= ~(31u << 7);
      texgen[1 + i] = info;
    }
    xf_words(state, 0x103f, texgen, 9);
  }
  uint32_t post[8];
  for (unsigned i = 0; i < 8; ++i) post[i] = st.xf_regs[0x12] & 1 ? st.xf_regs[0x50 + i] : 61;
  if (!prev || (pst->xf_regs[0x12] & 1) != (st.xf_regs[0x12] & 1) ||
      (dirty.xf_range(0x50, 8) && block_changed(st.xf_regs + 0x50, prev ? pst->xf_regs + 0x50 : nullptr, 8 * 4)))
    xf_words(state, 0x1050, post, 8);

  if (indices_changed) { cp(state, 0x30, st.matrix_index_a); cp(state, 0x40, st.matrix_index_b); }
  if (d.raw_size) {
    // Raw capture: the game's own vertex descriptor, format registers and vertex bytes go to Aurora's
    // command processor unchanged; each indexed array is the snapshotted slice the indices (rebased
    // at capture) address. No decode on the simulation thread, no re-encode here.
    check(d.raw_offset <= frame.raw_vertices.size() && d.raw_size <= frame.raw_vertices.size() - d.raw_offset, "invalid raw vertex range");
    if (!prev || prev->vcd_lo != d.vcd_lo || prev->vcd_hi != d.vcd_hi) { cp(state, 0x50, d.vcd_lo); cp(state, 0x60, d.vcd_hi); }
    const unsigned fmt = d.vtxfmt & 7;
    if (!prev || prev->vtxfmt != d.vtxfmt || prev->vat[0] != d.vat[0] || prev->vat[1] != d.vat[1] || prev->vat[2] != d.vat[2]) {
      cp(state, uint8_t(0x70 + fmt), d.vat[0]); cp(state, uint8_t(0x80 + fmt), d.vat[1]); cp(state, uint8_t(0x90 + fmt), d.vat[2]);
    }
    for (unsigned i = 0; i < 12; ++i) {
      const auto& a = d.arrays[i];
      out.arrays[i] = a.data.get();
      if (!a.data) { check(!a.stride || a.data, "indexed vertex array outside RAM"); continue; }
      check(a.data->size() >= a.bytes, "vertex array slice was not captured");
      // GX_AURORA_LOAD_ARRAYBASE | attribute: host pointer (u64), size (u32), little-endian flag (u8).
      state.push_back(0x50); u16(state, uint16_t(0x0010 | i));
      const uint64_t ptr = reinterpret_cast<uintptr_t>(a.data->data());
      u32(state, uint32_t(ptr >> 32)); u32(state, uint32_t(ptr));
      u32(state, uint32_t(a.data->size())); state.push_back(0);
      cp(state, uint8_t(0xB0 + i), a.stride);
    }
    out.primitive = uint8_t(d.primitive | fmt);
    out.vertices.assign(frame.raw_vertices.begin() + d.raw_offset, frame.raw_vertices.begin() + d.raw_offset + d.raw_size);
    out.vertex_stride = d.raw_size / d.vertex_count;
    return;
  }
  uint32_t vcd_lo = (d.components & 0x1ff) | (1u << 9), vcd_hi = 0;
  if (d.components & any_normal) vcd_lo |= 1u << 11;   // normal only, even for NBT sources
  if (d.components & VB_HAS_COL0) vcd_lo |= 1u << 13;
  if (d.components & VB_HAS_COL1) vcd_lo |= 1u << 15;
  for (unsigned i = 0; i < 8; ++i) if (d.components & (VB_HAS_UV0 << i)) vcd_hi |= 1u << (i * 2);
  if (!prev || prev->components != d.components) { cp(state, 0x50, vcd_lo); cp(state, 0x60, vcd_hi); }
  // F32 XYZ/normal (NBT when synthesized), RGBA8 colors, F32 ST. All fractions are zero.
  const uint32_t vat_a = 1 | (4u << 1) | (nbt ? 1u << 9 : 0) | (4u << 10) | (1u << 13) | (5u << 14) |
                         (1u << 17) | (5u << 18) | (1u << 21) | (4u << 22);
  cp(state, 0x70, vat_a);
  cp(state, 0x80, 9 | (9u << 9) | (9u << 18) | (9u << 27));
  cp(state, 0x90, (9u << 5) | (9u << 14) | (9u << 23));
  out.vertices.reserve(size_t(d.vertex_count) * sizeof(Vertex));
  for (unsigned i = 0; i < d.vertex_count; ++i) {
    const auto& v = frame.vertices[d.first_vertex + i];
    if (d.components & VB_HAS_POSMTXIDX) {
      soft(v.posmtx <= 27 && v.posmtx % 3 == 0, "per-vertex position matrix is outside Aurora's aligned range");
      out.vertices.push_back(v.posmtx);
    }
    for (unsigned t = 0; t < 8; ++t) if (d.components & (VB_HAS_TEXMTXIDX0 << t)) {
      soft(matrix_row(v.texmtx[t], false), "dynamic identity or unaligned texture matrix index is not covered");
      out.vertices.push_back(v.texmtx[t]);
    }
    for (float x : v.pos) { check(std::isfinite(x), "non-finite position"); f32(out.vertices, x); }
    if (d.components & any_normal) {
      for (float x : v.nrm) check(std::isfinite(x), "non-finite normal");
      for (int repeat = 0; repeat < (nbt ? 3 : 1); ++repeat) for (float x : v.nrm) f32(out.vertices, x);
    }
    if (d.components & VB_HAS_COL0) for (auto x : v.col0) out.vertices.push_back(x);
    if (d.components & VB_HAS_COL1) for (auto x : v.col1) out.vertices.push_back(x);
    for (unsigned t = 0; t < 8; ++t) if (d.components & (VB_HAS_UV0 << t))
      for (float x : v.uv[t]) { check(std::isfinite(x), "non-finite texture coordinate"); f32(out.vertices, x); }
  }
  out.vertex_stride = uint32_t(out.vertices.size() / d.vertex_count);
}

void validate_copy(const Frame& frame, size_t index, Extent extent) {
  const auto& c = frame.copies[index];
  const auto where = "copy " + std::to_string(index);
  if (!c.has_copy_state) fail(frame, where, "copy-time BP state was not captured");
  // Accept only the exact SDK unfiltered coefficients. Equal grouped sums are
  // not used as evidence that another sampling kernel is pixel-equivalent.
  // Aurora copies the EFB unfiltered (as Dolphin does with the copy filter disabled).
  // A non-default vertical filter, AA sample pattern or gamma is reported once and
  // otherwise ignored: it affects softness/brightness, never geometry or game state.
  if (c.filter0 != ((21u << 12) | (22u << 18)) || c.filter1 != 21 ||
      (c.genmode & (1u << 9)) || bits(c.copy_control, 7, 2)) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      std::fprintf(stderr, "aurora bridge: copy filter %06X/%06X gamma %u aa %u applied unfiltered\n",
                   c.filter0, c.filter1, bits(c.copy_control, 7, 2), (c.genmode >> 9) & 1);
    }
  }
  if (!c.src_w || !c.src_h || c.src_x >= extent.width || c.src_y >= extent.height ||
      c.src_w > extent.width - c.src_x || c.src_h > extent.height - c.src_y)
    fail(frame, where, "copy rectangle is outside the logical framebuffer");
  if (c.is_depth || (c.zcontrol & 7) > 1) fail(frame, where, "depth/unsupported-pixel-format copy");
  const bool full_rect = c.src_x == 0 && c.src_y == 0 && c.src_w == extent.width && c.src_h == extent.height;

  if (c.to_xfb) {
    if (!full_rect || c.y_scale != 1.f || c.half_scale || !c.clear ||
        (c.blendmode & 0x18) != 0x18 || !(c.zmode & 0x10))
      warn(frame, where, "XFB copy is not a full-size unscaled copy with full color/alpha/depth clear");
  } else {
    if (c.half_scale) warn(frame, where, "half-scale texture copy is copied at full size");
    const auto expected = texture_chain_bytes(c.src_w, c.src_h, efb_copy_texture_format(c), 1);
    const auto packed_stride = texture_level_bytes(c.src_w, 4, efb_copy_texture_format(c));   // one tile row in RAM
    if (c.dest_stride != packed_stride && c.dest_stride != packed_stride * 2)
      warn(frame, where, "strided texture-copy destination is copied as if packed");
    if (c.destination_before_copy.size() != expected)
      fail(frame, where, "copy-time destination bytes were not captured");
    if (c.dstalpha & 0x100) warn(frame, where, "destination-alpha copy override is not fidelity-validated");
  }
}
} // namespace

PreparedFrame prepare_frame(const Frame& frame, Extent extent) {
  PreparedFrame out;
  prepare_frame(frame, extent, out);
  return out;
}

void prepare_frame(const Frame& frame, Extent extent, PreparedFrame& out) {
  out.skipped_copies.clear();
  if (!extent.width || !extent.height || extent.width > EFB_WIDTH || extent.height > EFB_HEIGHT)
    fail(frame, "extent", "invalid logical framebuffer dimensions");
  out.sequence = frame.sequence;
  std::vector<bool> seen_draw(frame.draws.size()), seen_copy(frame.copies.size());
  unsigned xfb_count = 0;
  for (size_t i = 0; i < frame.commands.size(); ++i) {
    const auto& cmd = frame.commands[i];
    if (cmd.kind == FrameCommand::Draw) {
      if (cmd.index >= seen_draw.size() || seen_draw[cmd.index]) fail(frame, "commands", "invalid or repeated draw reference");
      seen_draw[cmd.index] = true;
    } else if (cmd.kind == FrameCommand::Copy) {
      if (cmd.index >= seen_copy.size() || seen_copy[cmd.index]) fail(frame, "commands", "invalid or repeated copy reference");
      seen_copy[cmd.index] = true;
      try {
        validate_copy(frame, cmd.index, extent);
      } catch (const CoverageError& error) {
        if (frame.copies[cmd.index].to_xfb) throw;
        warn(frame, "copy " + std::to_string(cmd.index), error.what());
        out.skipped_copies.push_back(cmd.index);
      }
      if (frame.copies[cmd.index].to_xfb && (++xfb_count != 1 || i + 1 != frame.commands.size()))
        fail(frame, "commands", "XFB must be the single final command");
    } else fail(frame, "commands", "unknown command kind");
  }
  if (xfb_count != 1 || std::find(seen_draw.begin(), seen_draw.end(), false) != seen_draw.end() ||
      std::find(seen_copy.begin(), seen_copy.end(), false) != seen_copy.end())
    fail(frame, "commands", "missing final XFB or unreferenced captured work");
  if (out.draws.size() < frame.draws.size()) out.draws.resize(frame.draws.size());   // grows only; buffers persist
  // Draw state is rebuilt in order from each draw's record (apply_state): `state` is the current draw's,
  // `emitted` that of the last draw handed to Aurora, which the next draw's register deltas compare with.
  // Both live as long as the thread (two 4.4 KB blocks, reused every frame).
  static thread_local std::unique_ptr<DrawState> state_storage(new DrawState()), emitted_storage(new DrawState());
  DrawState& state = *state_storage;
  DrawState& emitted = *emitted_storage;
  const DrawCall* previous = nullptr;   // last emitted draw since the frame start or the last copy
  Dirty dirty;                          // state parts written since `emitted` was last brought up to date
  for (const auto& cmd : frame.commands) {
    if (cmd.kind != FrameCommand::Draw) { previous = nullptr; continue; }   // copies rewrite BP state
    const size_t i = cmd.index;
    apply_state(state, frame.draws[i], frame);   // every draw, converted or not: the deltas chain through all of them
    dirty.add(frame.draws[i].delta);
    if (!previous) dirty.all = true;     // everything is emitted again: compare nothing, copy everything
    // Point at this frame's textures before anything can fail: the copy-history pass reads them for
    // every draw, including ones the bridge rejects below.
    for (unsigned k = 0; k < 8; ++k) out.draws[i].textures[k] = &frame.draws[i].textures[k];
    try {
      convert_draw(frame, i, extent, state, previous, previous ? &emitted : nullptr, dirty, out.draws[i]);
      // MELEE_BRIDGE_VERIFY=1 (bridge_verify=1): the same draw converted comparing every block, as before
      // the dirty tracking; any difference in the bytes handed to Aurora is logged.
      static const bool verify = [] { const char* v = std::getenv("MELEE_BRIDGE_VERIFY"); return v && v[0] == '1'; }();
      if (verify) {
        static thread_local Draw check_draw;
        static uint64_t checked = 0, mismatches = 0;
        Dirty everything;   // all = true
        convert_draw(frame, i, extent, state, previous, previous ? &emitted : nullptr, everything, check_draw);
        ++checked;
        if (check_draw.state != out.draws[i].state && ++mismatches <= 8)
          std::fprintf(stderr, "bridge-verify: draw %zu of frame %llu differs (%zu vs %zu state bytes)\n", i,
                       (unsigned long long)frame.sequence, out.draws[i].state.size(), check_draw.state.size());
        if ((checked & (checked - 1)) == 0 && checked >= 1024)
          std::fprintf(stderr, "bridge-verify: %llu draws checked, %llu differ\n", (unsigned long long)checked, (unsigned long long)mismatches);
      }
      previous = &frame.draws[i];
      dirty.copy(emitted, state);
      dirty.reset();
    } catch (const CoverageError& error) {
      // One draw the bridge cannot represent must not take the frame down.
      warn(frame, "draw " + std::to_string(i), error.what());
      out.draws[i].vertex_count = 0;   // the renderer skips it
      out.draws[i].state.clear(); out.draws[i].vertices.clear();
    }
  }
  out.copies = frame.copies;
  out.commands = frame.commands;
}

namespace {
void warn_history(const char* reason) {
  static std::set<std::string> reported;
  if (reported.insert(reason).second) std::fprintf(stderr, "aurora bridge: %s (reported once)\n", reason);
}
}  // namespace
CopyHistory advance_copy_history(const PreparedFrame& frame, CopyHistory history) {
  advance_copy_history_in_place(frame, history);
  return history;
}
// The in-place form: this pass only warns, so there is nothing to roll back, and copying the
// history map (up to 4096 nodes) every frame was most of the render thread's allocation.
void advance_copy_history_in_place(const PreparedFrame& frame, CopyHistory& history) {
  // Entries overlapping [lo, hi): the map is ordered by start address and no entry is longer than
  // max_bytes, so a backward walk from hi visits only candidates (a full walk per texture per draw
  // was 2% of the render thread).
  uint64_t max_bytes = 0;
  for (const auto& [address, record] : history) max_bytes = std::max<uint64_t>(max_bytes, record.bytes);
  const auto for_overlaps = [&](uint64_t lo, uint64_t hi, auto&& visit) {
    auto it = hi > UINT32_MAX ? history.end() : history.lower_bound(uint32_t(hi));
    while (it != history.begin()) {
      --it;
      if (uint64_t(it->first) + max_bytes <= lo) break;
      if (uint64_t(it->first) + it->second.bytes > lo) visit(it->first, it->second);
    }
  };
  for (const auto& cmd : frame.commands) {
    if (cmd.kind == FrameCommand::Copy) {
      const auto& c = frame.copies.at(cmd.index);
      const uint64_t bytes = c.to_xfb ? uint64_t(c.dest_stride) * c.src_h : uint64_t(c.dest_stride) * ((c.src_h + 3) / 4);
      if (!bytes || bytes > UINT32_MAX || uint64_t(c.dest_addr) + bytes > uint64_t(UINT32_MAX) + 1)
        { warn_history("invalid copy destination range"); continue; }
      for_overlaps(c.dest_addr, uint64_t(c.dest_addr) + bytes, [&](uint32_t address, const CopyRecord& previous) {
        if (address == c.dest_addr) {
          if (previous.width != c.src_w || previous.height != c.src_h || previous.bytes != bytes || previous.xfb != c.to_xfb)
            warn_history("copy shape change leaves an untracked destination tail");
        } else
          warn_history("overlapping copy destinations are not tracked");
      });
      history[c.dest_addr] = {c.src_w, c.src_h, uint32_t(bytes), c.to_xfb, c.destination_before_copy};
      max_bytes = std::max<uint64_t>(max_bytes, bytes);
      if (history.size() > 4096) history.erase(history.begin());
    } else {
      for (const TextureRef* tp : frame.draws.at(cmd.index).textures) {
        const TextureRef& t = *tp;
        if (!t.used) continue;
        const uint64_t end = uint64_t(t.addr) + texture_chain_bytes(t.width, t.height, t.format, t.mip_levels);
        for_overlaps(t.addr, end, [&](uint32_t address, const CopyRecord& copy) {
          if (address != t.addr || copy.xfb || copy.width != t.width || copy.height != t.height || t.format != 6 || t.mip_levels != 1)
            warn_history("unsupported texture view of an EFB/XFB copy");
          if (!t.data || copy.destination_bytes != t.data->image)
            warn_history("CPU-modified EFB-copy destination is not re-uploaded");
        });
      }
    }
  }
}
} // namespace gx::aurora_bridge
