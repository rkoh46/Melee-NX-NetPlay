#include "render_observer.h"
// GX command processor: FIFO -> register state -> captured frames.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_core.h"
#include "host.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace gx {

namespace {

BPMemory g_bp;
CPMemory g_cp;
XFMemory g_xf;
uint8_t g_tmem[1024 * 1024];
uint32_t g_bp_mask = 0xFFFFFF;
int32_t g_tev_colors[4][4], g_tev_kcolors[4][4];
Backend* g_backend = nullptr;
Frame g_frame;
TextureSnapshotCache g_texture_snapshots;
uint64_t g_frame_sequence = 0;
bool g_discontinuity = false;   // simulation thread only, like the rest of this file's state
std::vector<uint8_t> g_buf;   // storage only: g_buf_len bytes are valid (the vector is never shrunk per write)
}  // namespace
// The FIFO append state is public so the guest's stores to the write-gather pipe can land in the
// buffer from ppc::st32 itself (see fifo_write32 in ppc.h); the parse runs here when a command is complete.
uint8_t* g_fifo_data = nullptr; size_t g_fifo_cap = 0;
size_t g_fifo_len = 0;
size_t g_fifo_pos = 0;   // parsed prefix of the buffer
size_t g_fifo_need = 0;
namespace {
size_t& g_buf_len = g_fifo_len;
size_t& g_buf_pos = g_fifo_pos;
size_t& g_parse_need = g_fifo_need;
// Bytes the pending (incomplete) command at g_buf_pos needs before it can parse. The game writes the
// FIFO a few bytes at a time, so retrying a large vertex primitive on every write was quadratic.
inline size_t incomplete(size_t need) { g_parse_need = need; return 0; }
uint32_t g_cp_version = 1; // bumped on every CP register write: invalidates the vertex-descriptor cache
// Draw identity bookkeeping (reset per frame).
uint32_t g_dl_addr = 0, g_dl_draw_ordinal = 0, g_dl_call_ordinal = 0;
std::unordered_map<uint32_t, uint32_t> g_dl_calls;        // display list address -> calls this frame
std::unordered_map<uint64_t, uint32_t> g_immediate_draws;  // (tex0, count, prim) -> ordinal this frame
uint64_t g_commands = 0, g_draws = 0, g_vertices = 0;
uint32_t g_efb_copies = 0;

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }

// ---------------- vertex attribute description ----------------
struct AttrDesc {
  uint32_t type;      // 0 none, 1 direct, 2 index8, 3 index16
  uint32_t format;    // component format
  uint32_t count;     // elements flag
  uint32_t frac;
  uint32_t array;     // CP array index for indexed
};

// One indexed attribute's place in the vertex stream, for the raw capture (scan + rebase indices).
struct IndexedAttr {
  uint8_t attr;         // 0 pos, 1 nrm, 2 col0, 3 col1, 4..11 tex0..7 (also the CP array index)
  uint8_t offset;       // byte offset of its first index within the vertex
  uint8_t index_bytes;  // 1 or 2
  uint8_t index_count;  // 1, or 3 for NBT with separate indices
  uint32_t elem_bytes;  // bytes of one array element
};
struct VertexDesc {
  bool posmtx;
  bool texmtx[8];
  AttrDesc pos, nrm, col[2], tex[8];
  uint32_t nrm_index3;
  uint32_t size;      // bytes per vertex in the stream
  uint32_t vcd_lo, vcd_hi, vat[3];   // the CP registers this descriptor was built from
  IndexedAttr indexed[12]; uint32_t indexed_count;
};

uint32_t comp_bytes(uint32_t format) { static const uint32_t b[] = {1, 1, 2, 2, 4, 4, 4, 4}; return b[format & 7]; }

VertexDesc build_desc_uncached(uint32_t fmt) {
  VertexDesc d{};
  uint32_t lo = g_cp.vcd_lo(), hi = g_cp.vcd_hi();
  uint32_t a = g_cp.vat_a(fmt), b = g_cp.vat_b(fmt), c = g_cp.vat_c(fmt);
  d.posmtx = lo & 1;
  for (int i = 0; i < 8; ++i) d.texmtx[i] = (lo >> (1 + i)) & 1;
  d.pos = {bits(lo, 9, 2), bits(a, 1, 3), bits(a, 0, 1), bits(a, 4, 5), 0};
  d.nrm = {bits(lo, 11, 2), bits(a, 10, 3), bits(a, 9, 1), 0, 1};
  d.nrm_index3 = bits(a, 31, 1);
  d.col[0] = {bits(lo, 13, 2), bits(a, 14, 3), bits(a, 13, 1), 0, 2};
  d.col[1] = {bits(lo, 15, 2), bits(a, 18, 3), bits(a, 17, 1), 0, 3};
  d.tex[0] = {bits(hi, 0, 2), bits(a, 22, 3), bits(a, 21, 1), bits(a, 25, 5), 4};
  d.tex[1] = {bits(hi, 2, 2), bits(b, 1, 3), bits(b, 0, 1), bits(b, 4, 5), 5};
  d.tex[2] = {bits(hi, 4, 2), bits(b, 10, 3), bits(b, 9, 1), bits(b, 13, 5), 6};
  d.tex[3] = {bits(hi, 6, 2), bits(b, 19, 3), bits(b, 18, 1), bits(b, 22, 5), 7};
  d.tex[4] = {bits(hi, 8, 2), bits(b, 28, 3), bits(b, 27, 1), bits(c, 0, 5), 8};
  d.tex[5] = {bits(hi, 10, 2), bits(c, 6, 3), bits(c, 5, 1), bits(c, 9, 5), 9};
  d.tex[6] = {bits(hi, 12, 2), bits(c, 15, 3), bits(c, 14, 1), bits(c, 18, 5), 10};
  d.tex[7] = {bits(hi, 14, 2), bits(c, 24, 3), bits(c, 23, 1), bits(c, 27, 5), 11};
  d.vcd_lo = lo; d.vcd_hi = hi; d.vat[0] = a; d.vat[1] = b; d.vat[2] = c;
  d.indexed_count = 0;
  uint32_t size = 0;
  if (d.posmtx) size += 1;
  for (int i = 0; i < 8; ++i) if (d.texmtx[i]) size += 1;
  // Walks the attributes in stream order; indexed ones are noted with their offset and element size.
  auto attr = [&](const AttrDesc& x, uint8_t which, uint32_t direct, uint32_t index_count = 1) {
    if (x.type == 1) size += direct;
    else if (x.type == 2 || x.type == 3) {
      const uint8_t ib = x.type == 2 ? 1 : 2;
      d.indexed[d.indexed_count++] = {which, uint8_t(size), ib, uint8_t(index_count), direct / index_count};
      size += ib * index_count;
    }
  };
  attr(d.pos, 0, comp_bytes(d.pos.format) * (d.pos.count ? 3 : 2));
  if (d.nrm.type) {
    const uint32_t nvec = d.nrm.count ? 3 : 1;
    // One index addresses all three NBT vectors unless the NBT3 flag asks for three indices.
    const uint32_t nidx = (d.nrm.count && d.nrm_index3) ? 3 : 1;
    attr(d.nrm, 1, comp_bytes(d.nrm.format) * 3 * nvec, nidx);
  }
  static const uint32_t csize[] = {2, 3, 4, 2, 3, 4, 4, 4};
  for (int i = 0; i < 2; ++i) attr(d.col[i], uint8_t(2 + i), csize[d.col[i].format]);
  for (int i = 0; i < 8; ++i) attr(d.tex[i], uint8_t(4 + i), comp_bytes(d.tex[i].format) * (d.tex[i].count ? 2 : 1));
  d.size = size;
  return d;
}
// The descriptor for a vertex format only changes when a CP register does, so it is built once per
// CP state instead of once per FIFO write (the game streams a draw's vertices 4 bytes at a time, and
// the parser has to know the draw's length for every one of those writes).
const VertexDesc& build_desc(uint32_t fmt) {
  static VertexDesc cache[8];
  static uint32_t cache_version[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  fmt &= 7;
  if (cache_version[fmt] != g_cp_version) { cache[fmt] = build_desc_uncached(fmt); cache_version[fmt] = g_cp_version; }
  return cache[fmt];
}

// 2^-frac for the 5-bit vertex fraction field. The values are exact powers of two, identical to
// std::ldexp(1.0f, -frac), which cost a C library call for every component of every vertex.
constexpr float kFracScale[32] = {
    1.0f, 0x1p-1f, 0x1p-2f, 0x1p-3f, 0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f, 0x1p-8f, 0x1p-9f, 0x1p-10f, 0x1p-11f,
    0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f, 0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f, 0x1p-21f, 0x1p-22f,
    0x1p-23f, 0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f, 0x1p-31f};
inline float read_component(const uint8_t* p, uint32_t format, uint32_t frac) {
  const float scale = kFracScale[frac & 31];
  switch (format) {
    case 0: return (float)p[0] * scale;
    case 1: return (float)(int8_t)p[0] * scale;
    case 2: return (float)(uint16_t)be16(p) * scale;
    case 3: return (float)(int16_t)be16(p) * scale;
    default: { uint32_t u = be32(p); float f; std::memcpy(&f, &u, 4); return f; }
  }
}

void read_color(const uint8_t* p, uint32_t format, uint8_t out[4]) {
  switch (format) {
    case 0: { uint32_t v = be16(p); out[0] = (uint8_t)((v >> 11) * 255 / 31); out[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); out[2] = (uint8_t)((v & 31) * 255 / 31); out[3] = 255; break; }
    case 1: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 3: { uint32_t v = be16(p); out[0] = (uint8_t)(((v >> 12) & 15) * 17); out[1] = (uint8_t)(((v >> 8) & 15) * 17); out[2] = (uint8_t)(((v >> 4) & 15) * 17); out[3] = (uint8_t)((v & 15) * 17); break; }
    case 4: { uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      out[0] = (uint8_t)(((v >> 18) & 63) * 255 / 63); out[1] = (uint8_t)(((v >> 12) & 63) * 255 / 63); out[2] = (uint8_t)(((v >> 6) & 63) * 255 / 63); out[3] = (uint8_t)((v & 63) * 255 / 63); break; }
    default: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; break;
  }
}

const uint8_t* array_ptr(uint32_t array, uint32_t index) {
  uint32_t base = g_cp.array_base(array), stride = g_cp.array_stride(array);
  return host::ptr(0x80000000u | ((base + stride * index) & 0x01FFFFFFu));
}

// Decodes `count` vertices of format `fmt` from `src` into the frame. Returns components mask.
// Per-draw decode plan: everything that is constant for a draw (formats, component sizes, fixed-point
// scales, indexed-array bases and strides, the matrix-index defaults) is resolved once here, so the
// per-vertex loop is straight-line loads. The arithmetic is exactly read_component's: an integer
// converted to float and multiplied by the same power-of-two scale, or the raw IEEE float.
namespace {
struct AttrPlan {
  uint32_t type = 0;       // 0 none, 1 direct, 2 index8, 3 index16
  uint32_t format = 0, cb = 0, n = 0, direct_size = 0;
  float scale = 1.0f;
  uint32_t array_base = 0, stride = 0;   // indexed arrays (guest addresses, resolved per index like array_ptr)
  inline const uint8_t* fetch(const uint8_t*& p) const {
    if (type == 1) { const uint8_t* q = p; p += direct_size; return q; }
    uint32_t index; if (type == 2) { index = *p; p += 1; } else { index = be16(p); p += 2; }
    return indexed(index);
  }
  inline const uint8_t* indexed(uint32_t index) const {
    // array_ptr's arithmetic without the call: an index outside RAM yields nullptr (the caller skips it).
    const uint32_t off = (array_base + stride * index) & 0x01FFFFFFu;
    return off < ppc::RAM_SIZE ? host::ram + off : nullptr;
  }
  inline float component(const uint8_t* q, uint32_t k) const {
    const uint8_t* c = q + k * cb;
    switch (format) {
      case 0: return (float)c[0] * scale;
      case 1: return (float)(int8_t)c[0] * scale;
      case 2: return (float)(uint16_t)be16(c) * scale;
      case 3: return (float)(int16_t)be16(c) * scale;
      default: { uint32_t u = be32(c); float f; std::memcpy(&f, &u, 4); return f; }
    }
  }
};
void plan_set(AttrPlan& a, const AttrDesc& x, uint32_t n) {
  a.type = x.type; a.format = x.format; a.cb = comp_bytes(x.format); a.n = n; a.direct_size = a.cb * n;
  a.scale = std::ldexp(1.0f, -(int)x.frac);
  if (a.type == 2 || a.type == 3) { a.array_base = g_cp.array_base(x.array); a.stride = g_cp.array_stride(x.array); }
}
}  // namespace

uint32_t decode_vertices(const VertexDesc& d, const uint8_t* src, uint32_t count, uint32_t fmt) {
  uint32_t components = 0;
  const uint32_t mia = g_cp.matrix_index_a(), mib = g_cp.matrix_index_b();
  // ---- plan
  uint8_t texmtx_default[8];
  for (int i = 0; i < 8; ++i) texmtx_default[i] = (uint8_t)(i < 4 ? bits(mia, 6 + 6 * i, 6) : bits(mib, 6 * (i - 4), 6));
  const uint8_t posmtx_default = (uint8_t)(mia & 63);
  if (d.posmtx) components |= VB_HAS_POSMTXIDX;
  for (int i = 0; i < 8; ++i) if (d.texmtx[i]) components |= VB_HAS_TEXMTXIDX0 << i;
  AttrPlan pos; plan_set(pos, d.pos, d.pos.count ? 3 : 2);
  AttrPlan nrm; uint32_t nrm_index_bytes = 0, nrm_direct_bytes = 0;
  if (d.nrm.type) {
    const uint32_t frac = d.nrm.format == 1 ? 6 : d.nrm.format == 3 ? 14 : d.nrm.format == 0 ? 7 : d.nrm.format == 2 ? 15 : 0;
    AttrDesc nd = d.nrm; nd.frac = frac;
    plan_set(nrm, nd, 3);
    const uint32_t nvec = d.nrm.count ? 3 : 1;
    if (nvec > 1) components |= VB_UNCAPTURED_NBT;
    nrm_direct_bytes = nrm.cb * 3 * nvec;
    nrm_index_bytes = d.nrm.type == 2 ? ((d.nrm.count && d.nrm_index3) ? 3 : 1) : ((d.nrm.count && d.nrm_index3) ? 6 : 2);
    components |= VB_HAS_NRM0;
  }
  static const uint32_t csize[] = {2, 3, 4, 2, 3, 4, 4, 4};
  AttrPlan col[2];
  for (int i = 0; i < 2; ++i) {
    if (!d.col[i].type) continue;
    plan_set(col[i], d.col[i], 1); col[i].direct_size = csize[d.col[i].format];
    components |= i ? VB_HAS_COL1 : VB_HAS_COL0;
  }
  AttrPlan tex[8];
  for (int i = 0; i < 8; ++i) {
    if (!d.tex[i].type) continue;
    plan_set(tex[i], d.tex[i], d.tex[i].count ? 2 : 1);
    components |= VB_HAS_UV0 << i;
  }
  // ---- vertices, written in place. The vector does not zero new elements (VertexVector), so every
  // field a consumer may read for this draw's components is written here: the third position
  // component of XY draws, the second coordinate of S-only texgens, missing array entries.
  // Only the attributes this format carries are visited per vertex (compact lists), and the
  // matrix-index bytes are handled by count rather than eight tests a vertex.
  uint8_t texmtx_slots[8]; unsigned texmtx_count = 0;
  for (int i = 0; i < 8; ++i) if (d.texmtx[i]) texmtx_slots[texmtx_count++] = uint8_t(i);
  uint8_t col_slots[2]; unsigned col_count = 0;
  for (int i = 0; i < 2; ++i) if (d.col[i].type) col_slots[col_count++] = uint8_t(i);
  uint8_t tex_slots[8]; unsigned tex_count = 0;
  for (int i = 0; i < 8; ++i) if (d.tex[i].type) tex_slots[tex_count++] = uint8_t(i);
  const size_t first = g_frame.vertices.size();
  g_frame.vertices.resize(first + count);
  Vertex* outv = g_frame.vertices.data() + first;
  const uint8_t* p = src;
  for (uint32_t v = 0; v < count; ++v, p = src + (size_t)(v) * d.size) {
    Vertex& out = outv[v];
    out.posmtx = d.posmtx ? *p++ : posmtx_default;
    std::memcpy(out.texmtx, texmtx_default, 8);
    for (unsigned j = 0; j < texmtx_count; ++j) out.texmtx[texmtx_slots[j]] = *p++;
    out.pos[2] = 0.f;
    if (const uint8_t* q = pos.fetch(p)) for (uint32_t k = 0; k < pos.n; ++k) out.pos[k] = pos.component(q, k);
    else out.pos[0] = out.pos[1] = 0.f;
    if (d.nrm.type) {
      const uint8_t* q;
      if (d.nrm.type == 1) { q = p; p += nrm_direct_bytes; }
      else if (d.nrm.type == 2) { q = nrm.indexed(*p); p += nrm_index_bytes; }
      else { q = nrm.indexed(be16(p)); p += nrm_index_bytes; }
      if (q) for (int k = 0; k < 3; ++k) out.nrm[k] = nrm.component(q, k);
      else out.nrm[0] = out.nrm[1] = out.nrm[2] = 0.f;
    }
    for (unsigned j = 0; j < col_count; ++j) {
      const unsigned i = col_slots[j];
      const uint8_t* q = col[i].fetch(p);
      uint8_t* dst = i ? out.col1 : out.col0;
      if (q) read_color(q, d.col[i].format, dst);
      else dst[0] = dst[1] = dst[2] = dst[3] = 0;
      if (!d.col[i].count) dst[3] = 255;
    }
    for (unsigned j = 0; j < tex_count; ++j) {
      const unsigned i = tex_slots[j];
      out.uv[i][1] = 0.f;
      if (const uint8_t* q = tex[i].fetch(p)) for (uint32_t k = 0; k < tex[i].n; ++k) out.uv[i][k] = tex[i].component(q, k);
      else out.uv[i][0] = 0.f;
    }
  }
  (void)fmt;
  return components;
}

// Components mask from the descriptor alone (what decode_vertices reports), for the raw path.
uint32_t components_of(const VertexDesc& d) {
  uint32_t components = 0;
  if (d.posmtx) components |= VB_HAS_POSMTXIDX;
  for (int i = 0; i < 8; ++i) if (d.texmtx[i]) components |= VB_HAS_TEXMTXIDX0 << i;
  if (d.nrm.type) { components |= VB_HAS_NRM0; if (d.nrm.count) components |= VB_HAS_NRM1 | VB_HAS_NRM2; }
  for (int i = 0; i < 2; ++i) if (d.col[i].type) components |= i ? VB_HAS_COL1 : VB_HAS_COL0;
  for (int i = 0; i < 8; ++i) if (d.tex[i].type) components |= VB_HAS_UV0 << i;
  return components;
}

// Raw capture: the draw's vertex bytes are copied as the game wrote them, and each indexed array is
// captured once per frame from its base up to the highest element any draw addressed (a later draw
// reaching further extends the copy). Pending until record_draw stores it in the DrawCall. One memcpy
// per array per frame replaced a hashed, compared snapshot per draw, which cost as much as decoding.
struct RawPending { uint32_t fmt = 0, offset = 0, size = 0; VertexArrayRef arrays[12]; bool valid = false; };
RawPending g_raw_pending;
bool g_state_deltas = false;   // set by init(): the backend rebuilds draw state from deltas (apply_state)
StateDelta g_dirty;            // parts of the live state written since the last recorded draw
bool g_raw_vertices = false;   // set by init(): renderers that consume the hardware format ask for it
std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> g_frame_arrays;   // (base, stride) -> this frame's copy
// The last array each attribute used (consecutive draws of one model share them), checked before the map.
struct LastArray { uint64_t key = ~uint64_t(0); std::shared_ptr<std::vector<uint8_t>>* slot = nullptr; };
LastArray g_last_array[12];
// Array buffers outlive the frame on the render thread; a pooled buffer whose only owner is the pool
// again (use_count 1: nothing else can take a new reference, since only this thread hands them out) is
// reused with its capacity, instead of one allocation per array per frame freed on the other thread.
std::vector<std::shared_ptr<std::vector<uint8_t>>> g_array_pool;
size_t g_array_pool_next = 0;
std::shared_ptr<std::vector<uint8_t>> pooled_array() {
  for (size_t n = 0; n < g_array_pool.size(); ++n) {
    auto& p = g_array_pool[g_array_pool_next];
    g_array_pool_next = (g_array_pool_next + 1) % g_array_pool.size();
    if (p.use_count() == 1) { p->clear(); return p; }
  }
  if (g_array_pool.size() < 4096) { g_array_pool.push_back(std::make_shared<std::vector<uint8_t>>()); return g_array_pool.back(); }
  return std::make_shared<std::vector<uint8_t>>();
}

void capture_raw_vertices(const VertexDesc& d, uint32_t fmt, const uint8_t* src, uint32_t count) {
  auto& out = g_frame.raw_vertices;
  const size_t at = out.size(), bytes = size_t(count) * d.size;
  out.insert(out.end(), src, src + bytes);   // one copy (resize then memcpy zero-filled the bytes first)
  g_raw_pending = RawPending{};
  g_raw_pending.fmt = fmt; g_raw_pending.offset = uint32_t(at); g_raw_pending.size = uint32_t(bytes); g_raw_pending.valid = true;
  const uint8_t* verts = out.data() + at;
  for (uint32_t k = 0; k < d.indexed_count; ++k) {
    const IndexedAttr& ia = d.indexed[k];
    uint32_t hi = 0;
    const uint8_t* p0 = verts + ia.offset;
    const size_t step = d.size;
    if (ia.index_count == 1 && ia.index_bytes == 1) {          // the common shapes, without the inner loop
      for (uint32_t v = 0; v < count; ++v) hi = std::max(hi, uint32_t(p0[size_t(v) * step]));
    } else if (ia.index_count == 1) {
      for (uint32_t v = 0; v < count; ++v) { const uint8_t* p = p0 + size_t(v) * step; hi = std::max(hi, uint32_t(p[0]) << 8 | p[1]); }
    } else {
      for (uint32_t v = 0; v < count; ++v) {
        const uint8_t* p = p0 + size_t(v) * step;
        for (uint32_t j = 0; j < ia.index_count; ++j, p += ia.index_bytes) hi = std::max(hi, ia.index_bytes == 1 ? uint32_t(*p) : be16(p));
      }
    }
    const uint32_t base = g_cp.array_base(ia.attr) & 0x01FFFFFFu, stride = g_cp.array_stride(ia.attr);
    const uint32_t need = hi * stride + ia.elem_bytes;
    auto& ref = g_raw_pending.arrays[ia.attr];
    if (base >= ppc::RAM_SIZE || need > ppc::RAM_SIZE - base) { ref.stride = stride ? stride : 1; continue; }   // outside RAM: unbound, the bridge skips the draw
    const uint64_t key = uint64_t(base) | (uint64_t(stride) << 32);
    LastArray& last = g_last_array[ia.attr];
    if (last.key != key || !last.slot) { last.key = key; last.slot = &g_frame_arrays[key]; }
    auto& slot = *last.slot;
    if (!slot) slot = pooled_array();
    if (slot->size() < need) {
      const size_t had = slot->size();
      slot->insert(slot->end(), host::ram + base + had, host::ram + base + need);
    }
    ref.data = slot; ref.stride = stride; ref.first_index = 0; ref.count = hi + 1; ref.bytes = need;
  }
}

void apply_state_impl(DrawState& into, const DrawCall& from) {
  const StateDelta& d = from.delta;
  if (d.full) { into = static_cast<const DrawState&>(from); return; }
  for (unsigned w = 0; w < 4; ++w)
    for (uint64_t m = d.bp[w]; m; m &= m - 1) { const unsigned r = w * 64 + __builtin_ctzll(m); into.bp.reg[r] = from.bp.reg[r]; }
  for (uint64_t m = d.pos; m; m &= m - 1) std::memcpy(into.posMatrices + __builtin_ctzll(m) * 4, from.posMatrices + __builtin_ctzll(m) * 4, 16);
  for (uint32_t m = d.nrm; m; m &= m - 1) std::memcpy(into.normalMatrices + __builtin_ctz(m) * 3, from.normalMatrices + __builtin_ctz(m) * 3, 12);
  for (uint64_t m = d.post; m; m &= m - 1) std::memcpy(into.postMatrices + __builtin_ctzll(m) * 4, from.postMatrices + __builtin_ctzll(m) * 4, 16);
  for (unsigned m = d.lights; m; m &= m - 1) std::memcpy(into.lights[__builtin_ctz(m)], from.lights[__builtin_ctz(m)], 64);
  for (unsigned w = 0; w < 2; ++w)
    for (uint64_t m = d.xf[w]; m; m &= m - 1) { const unsigned r = w * 64 + __builtin_ctzll(m); into.xf_regs[r] = from.xf_regs[r]; }
  into.matrix_index_a = from.matrix_index_a; into.matrix_index_b = from.matrix_index_b;
  std::memcpy(into.tev_colors, from.tev_colors, sizeof into.tev_colors);
  std::memcpy(into.tev_kcolors, from.tev_kcolors, sizeof into.tev_kcolors);
}

// ---------------- draw snapshot ----------------
void snapshot_textures(DrawCall& dc) {
  host::SimCostScope cost(host::SIM_SNAPSHOT);
  uint32_t stages = g_bp.numtevstages() + 1;
  for (uint32_t s = 0; s < stages; ++s) {
    if (!g_bp.order_enable(s)) continue;
    int map = g_bp.order_texmap(s);
    TextureRef& t = dc.textures[map];
    if (t.used) continue;
    t.used = true;
    uint32_t i0 = g_bp.teximage0(map);
    t.width = (i0 & 0x3FF) + 1;
    t.height = ((i0 >> 10) & 0x3FF) + 1;
    t.format = (i0 >> 20) & 0xF;
    t.addr = (g_bp.teximage3(map) & 0xFFFFFF) << 5;
    uint32_t tl = g_bp.textlut(map);
    t.tlut_addr = (tl & 0x3FF) << 9;
    t.tlut_format = (tl >> 10) & 3;
    t.mode0 = g_bp.texmode0(map);
    t.mode1 = g_bp.texmode1(map);
    uint32_t min_filter = (t.mode0 >> 5) & 7;
    // GX low two filter bits select mip filtering; high bit selects minification.
    bool use_mips = (min_filter & 3) != 0;
    uint32_t max_lod = (t.mode1 >> 8) & 0xFF;
    t.mip_levels = texture_mip_count(t.width, t.height, use_mips ? ((max_lod + 15) / 16) + 1 : 1);
  }
  // Indirect stages reference textures too.
  uint32_t ind = g_bp.numindstages();
  for (uint32_t i = 0; i < ind; ++i) {
    int map = bits(g_bp.iref(), 6 * i, 3);
    TextureRef& t = dc.textures[map];
    if (t.used) continue;
    t.used = true;
    uint32_t i0 = g_bp.teximage0(map);
    t.width = (i0 & 0x3FF) + 1; t.height = ((i0 >> 10) & 0x3FF) + 1; t.format = (i0 >> 20) & 0xF;
    t.addr = (g_bp.teximage3(map) & 0xFFFFFF) << 5;
    uint32_t tl = g_bp.textlut(map);
    t.tlut_addr = (tl & 0x3FF) << 9; t.tlut_format = (tl >> 10) & 3;
    t.mode0 = g_bp.texmode0(map); t.mode1 = g_bp.texmode1(map);
    t.mip_levels = 1;
  }
  for (TextureRef& t : dc.textures) {
    if (!t.used) continue;
    uint32_t total = texture_chain_bytes(t.width, t.height, t.format, t.mip_levels);
    uint32_t offset = t.addr & 0x3FFFFFFFu;
    uint32_t palette_bytes = t.format == 8 ? 32 : t.format == 9 ? 512 : t.format == 10 ? 32768 : 0;
    if (offset >= ppc::RAM_SIZE || total > ppc::RAM_SIZE - offset ||
        t.tlut_addr > sizeof g_tmem || palette_bytes > sizeof g_tmem - t.tlut_addr)
      host::die("GX texture range invalid: %08X+%X, palette %X+%X", t.addr, total, t.tlut_addr, palette_bytes);
    t.data = g_texture_snapshots.capture(host::ram + offset, total, g_tmem + t.tlut_addr, palette_bytes);
  }
}

void record_draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t components) {
  host::SimCostScope record_cost(host::SIM_RECORD);
  // Filled in place: the recycled frame buffers keep their storage warm between frames, and the
  // 5 KB move of a stack copy was a measurable share of the simulation thread on the console.
  g_frame.draws.emplace_back(DrawCall::SkipInit{});
  DrawCall& dc = g_frame.draws.back();
  dc.primitive = primitive;
  dc.first_vertex = first;
  dc.vertex_count = count;
  dc.components = components;
  if (g_raw_pending.valid) {
    dc.vtxfmt = g_raw_pending.fmt; dc.raw_offset = g_raw_pending.offset; dc.raw_size = g_raw_pending.size;
    const VertexDesc& desc = build_desc(dc.vtxfmt);
    dc.vcd_lo = desc.vcd_lo; dc.vcd_hi = desc.vcd_hi; dc.vat[0] = desc.vat[0]; dc.vat[1] = desc.vat[1]; dc.vat[2] = desc.vat[2];
    for (int i = 0; i < 12; ++i) dc.arrays[i] = std::move(g_raw_pending.arrays[i]);
    g_raw_pending.valid = false;
  } else {
    dc.raw_size = 0;
    for (int i = 0; i < 12; ++i) dc.arrays[i] = VertexArrayRef{};
  }
  if (g_state_deltas && g_frame.draws.size() > 1) {
    // Only what changed since the previous draw of this frame: on a typical draw a position and a
    // normal matrix and a few BP registers, instead of ~4.4 KB copied for every one of ~1,500 draws.
    dc.delta = g_dirty; dc.delta.full = false;
    for (unsigned w = 0; w < 4; ++w)
      for (uint64_t m = g_dirty.bp[w]; m; m &= m - 1) { const unsigned r = w * 64 + __builtin_ctzll(m); dc.bp.reg[r] = g_bp.reg[r]; }
    for (uint64_t m = g_dirty.pos; m; m &= m - 1) std::memcpy(dc.posMatrices + __builtin_ctzll(m) * 4, g_xf.posMatrices + __builtin_ctzll(m) * 4, 16);
    for (uint32_t m = g_dirty.nrm; m; m &= m - 1) std::memcpy(dc.normalMatrices + __builtin_ctz(m) * 3, g_xf.normalMatrices + __builtin_ctz(m) * 3, 12);
    for (uint64_t m = g_dirty.post; m; m &= m - 1) std::memcpy(dc.postMatrices + __builtin_ctzll(m) * 4, g_xf.postMatrices + __builtin_ctzll(m) * 4, 16);
    for (unsigned m = g_dirty.lights; m; m &= m - 1) std::memcpy(dc.lights[__builtin_ctz(m)], &g_xf.lights[__builtin_ctz(m)], 64);
    for (unsigned w = 0; w < 2; ++w)
      for (uint64_t m = g_dirty.xf[w]; m; m &= m - 1) { const unsigned r = w * 64 + __builtin_ctzll(m); dc.xf_regs[r] = g_xf.raw[0x1000 + r]; }
  } else if (g_state_deltas) {
    // The frame's first draw carries the whole state the deltas build on.
    dc.delta = StateDelta{};
    dc.bp = g_bp;
    std::memcpy(dc.posMatrices, g_xf.posMatrices, sizeof dc.posMatrices);
    std::memcpy(dc.normalMatrices, g_xf.normalMatrices, sizeof dc.normalMatrices);
    std::memcpy(dc.postMatrices, g_xf.postMatrices, sizeof dc.postMatrices);
    std::memcpy(dc.lights, g_xf.lights, sizeof dc.lights);
    std::memcpy(dc.xf_regs, &g_xf.raw[0x1000], sizeof dc.xf_regs);
  } else {
    dc.delta = StateDelta{};
    dc.bp = g_bp;
    std::memcpy(dc.posMatrices, g_xf.posMatrices, sizeof dc.posMatrices);
    std::memcpy(dc.normalMatrices, g_xf.normalMatrices, sizeof dc.normalMatrices);
    std::memcpy(dc.xf_regs, &g_xf.raw[0x1000], sizeof dc.xf_regs);
    // Post-transform matrices and lights are 1.5 KB of the draw and most draws use neither; the
    // renderer reads them under the same tests (fill_vs_constants), so skipped copies are never read.
    if (dc.xf_regs[0x12] & 1) std::memcpy(dc.postMatrices, g_xf.postMatrices, sizeof dc.postMatrices);
    bool lit = false;
    for (uint32_t j = 0; j < (dc.xf_regs[0x09] & 3); ++j) lit = lit || lit_enable(dc.xf_regs[0x0E + j]) || lit_enable(dc.xf_regs[0x10 + j]);
    if (lit) std::memcpy(dc.lights, g_xf.lights, sizeof dc.lights);
  }
  g_dirty = StateDelta{}; g_dirty.full = false;
  dc.matrix_index_a = g_cp.matrix_index_a();
  dc.matrix_index_b = g_cp.matrix_index_b();
  std::memcpy(dc.tev_colors, g_tev_colors, sizeof g_tev_colors);
  std::memcpy(dc.tev_kcolors, g_tev_kcolors, sizeof g_tev_kcolors);
  snapshot_textures(dc);
  if (g_state_deltas) {
    // Draw identities pair draws across frames for the Metal renderer's sub-frames; Aurora never reads
    // them, and the per-frame hash maps behind them allocated nodes on every draw.
    dc.identity = 0;
  } else if (g_dl_addr) {
    dc.identity = hash_bytes(&g_dl_addr, 4) ^ ((uint64_t)g_dl_call_ordinal << 40) ^ ((uint64_t)g_dl_draw_ordinal << 20) ^ 1;
    ++g_dl_draw_ordinal;
  } else {
    uint64_t k = ((uint64_t)dc.textures[0].addr << 32) ^ ((uint64_t)count << 8) ^ primitive;
    uint32_t ordinal = g_immediate_draws[k]++;
    dc.identity = hash_bytes(&k, 8) ^ ((uint64_t)ordinal << 44) ^ 2;
  }
  { host::SimCostScope cost(host::SIM_OBSERVE);
    dc.identity = observed_draw_identity(dc.identity, dc.object_generation);
    dc.owner_player = observed_owner();   // SkipInit above means this has to be written every draw
    dc.skinned = observed_skinned();
    dc.authored_pose = capture_authored_pose(); }
  g_frame.commands.push_back({FrameCommand::Draw, (uint32_t)g_frame.draws.size() - 1});
  ++g_draws;
  g_vertices += count;
}

// ---------------- BP side effects ----------------
void bp_write(uint32_t value) {
  uint8_t r = (uint8_t)(value >> 24);
  uint32_t v = value & 0xFFFFFF;
  if (r == BP_BP_MASK) { g_bp_mask = v; return; }
  uint32_t masked = (g_bp.reg[r] & ~g_bp_mask) | (v & g_bp_mask);
  g_bp_mask = 0xFFFFFF;
  g_bp.reg[r] = masked;
  g_dirty.bp[r >> 6] |= uint64_t(1) << (r & 63);
  if (r >= BP_TEV_COLOR_RA && r <= BP_TEV_COLOR_RA + 7) {
    // TEV color/konst registers share BP slots; the type bit selects the destination at write time.
    int idx = (r - BP_TEV_COLOR_RA) / 2;
    bool bg = r & 1;
    int32_t (*dst)[4] = (masked & (1 << 23)) ? g_tev_kcolors : g_tev_colors;
    if (bg) { dst[idx][2] = sbits(masked, 0, 11); dst[idx][1] = sbits(masked, 12, 11); }
    else { dst[idx][0] = sbits(masked, 0, 11); dst[idx][3] = sbits(masked, 12, 11); }
    return;
  }
  switch (r) {
    case BP_SETDRAWDONE:
      if (masked & 2) host::set_pe_finish_pending();
      break;
    case BP_PE_TOKEN_INT_ID:
      host::set_pe_token_pending((uint16_t)masked);
      break;
    case BP_LOADTLUT1: {
      uint32_t tmem_addr = (masked & 0x3FF) << 9;
      uint32_t count = (masked & 0x1FFC00) >> 5;
      uint32_t src = (g_bp.reg[BP_LOADTLUT0] << 5) & 0x01FFFFFF;
      if (tmem_addr + count <= sizeof g_tmem) std::memcpy(g_tmem + tmem_addr, host::ptr(0x80000000u | src, count), count);
      break;
    }
    case BP_TRIGGER_EFB_COPY: {
      EfbCopy c{};
      capture_copy_state(c, g_bp);
      c.dest_addr = g_bp.reg[BP_EFB_ADDR] << 5;
      c.dest_stride = g_bp.reg[BP_MIPMAP_STRIDE] << 5;
      uint32_t tl = g_bp.reg[BP_EFB_TL], br = g_bp.reg[BP_EFB_BR];
      c.src_x = tl & 0x3FF; c.src_y = (tl >> 10) & 0x3FF;
      c.src_w = (br & 0x3FF) + 1; c.src_h = ((br >> 10) & 0x3FF) + 1;
      uint32_t tpf = bits(masked, 3, 4);
      c.format = tpf / 2 + (tpf & 1) * 8;
      c.to_xfb = bits(masked, 14, 1);
      c.clear = bits(masked, 11, 1);
      c.intensity = bits(masked, 15, 1);
      c.half_scale = bits(masked, 9, 1);
      c.is_depth = (g_bp.zcontrol() & 7) == 3;
      c.clear_color = ((g_bp.reg[BP_CLEAR_AR] & 0xFF) << 24) | ((g_bp.reg[BP_CLEAR_AR] & 0xFF00) << 8) |
                      ((g_bp.reg[BP_CLEAR_GB] & 0xFF00) >> 8 << 8) | (g_bp.reg[BP_CLEAR_GB] & 0xFF);
      // clear_color layout: A(31..24) R(23..16) G(15..8) B(7..0)
      c.clear_color = ((g_bp.reg[BP_CLEAR_AR] >> 8) & 0xFF) << 24 | (g_bp.reg[BP_CLEAR_AR] & 0xFF) << 16 |
                      ((g_bp.reg[BP_CLEAR_GB] >> 8) & 0xFF) << 8 | (g_bp.reg[BP_CLEAR_GB] & 0xFF);
      c.clear_z = g_bp.reg[BP_CLEAR_Z] & 0xFFFFFF;
      uint32_t yscale = g_bp.reg[BP_COPYYSCALE];
      c.y_scale = bits(masked, 10, 1) ? 256.0f / (float)yscale : (float)yscale / 256.0f;
      // Every texture copy's destination is captured (the renderer compares later snapshots of the
      // same bytes to detect CPU writes). This used to be RGBA8 copies only, and the Aurora bridge
      // refused copies without the capture: Melee's I4 shadow maps and RGB5A3/RGB565 copies (the
      // off-screen magnifier, Pokémon Stadium's screen) were never copied and sampled stale RAM.
      if (!c.to_xfb && !c.is_depth && !c.half_scale) {
        const uint64_t size = texture_chain_bytes(c.src_w, c.src_h, efb_copy_texture_format(c), 1);
        const uint32_t offset = c.dest_addr & 0x3fffffffu;
        if (offset < ppc::RAM_SIZE && size <= ppc::RAM_SIZE - offset)
          c.destination_before_copy.assign(host::ram + offset, host::ram + offset + size);
      }
      g_frame.copies.push_back(c);
      g_frame.commands.push_back({FrameCommand::Copy, (uint32_t)g_frame.copies.size() - 1});
      ++g_efb_copies;
      if (c.to_xfb) {
        g_frame.sequence = ++g_frame_sequence;
        g_frame.scene_major = host::rd8(0x80479D30);
        g_frame.scene_minor = host::rd8(0x80479D33);
        {
          static uint16_t last_scene = 0xFFFF;
          const uint16_t scene = (uint16_t)(g_frame.scene_major << 8 | g_frame.scene_minor);
          if (scene != last_scene) { host::log("scene: major %02X minor %02X (frame %llu)", g_frame.scene_major, g_frame.scene_minor, (unsigned long long)g_frame.sequence); last_scene = scene; }
        }
        g_frame.time = host::now_seconds(); // completed snapshot availability anchors presentation
        g_frame.discontinuous = g_discontinuity;
        g_discontinuity = false;
        if (g_backend) g_backend->submit_and_recycle(g_frame);   // hands over the buffers, returns recycled ones
        else g_frame.clear();
        g_texture_snapshots.end_frame();
        g_frame_arrays.clear();   // the frame's draws keep their shared copies alive
        for (auto& last : g_last_array) last = LastArray{};
        g_dl_calls.clear(); g_immediate_draws.clear(); finish_observed_frame();
      }
      break;
    }
    default:
      break;
  }
}

// ---------------- XF ----------------
void xf_load(uint32_t address, uint32_t count, const uint8_t* data) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t a = address + i;
    if (a < 0x1058) {
      g_xf.raw[a] = be32(data + i * 4);
      if (a < 0x100) g_dirty.pos |= uint64_t(1) << (a >> 2);
      else if (a >= 0x400 && a < 0x460) g_dirty.nrm |= 1u << ((a - 0x400) / 3);
      else if (a >= 0x500 && a < 0x600) g_dirty.post |= uint64_t(1) << ((a - 0x500) >> 2);
      else if (a >= 0x600 && a < 0x680) g_dirty.lights |= uint8_t(1u << ((a - 0x600) >> 4));
      else if (a >= 0x1000) g_dirty.xf[(a - 0x1000) >> 6] |= uint64_t(1) << ((a - 0x1000) & 63);
    }
  }
}

void xf_indexed_load(uint32_t op, uint32_t value) {
  uint32_t index = value >> 16, address = value & 0xFFF, size = ((value >> 12) & 0xF) + 1;
  uint32_t array = 12 + (op - 0x20) / 8;
  const uint8_t* src = array_ptr(array, index);
  xf_load(address, size, src);
}

// ---------------- command parsing ----------------
size_t parse_command(const uint8_t* d, size_t len);

void run_display_list(uint32_t addr, uint32_t size) {
  if ((addr & 0x3FFFFFFFu) + size > 0x01800000u) { host::log("gx: display list outside RAM %08X+%X", addr, size); return; }
  const uint8_t* p = host::ptr(addr, size);
  uint32_t saved_addr = g_dl_addr, saved_draw = g_dl_draw_ordinal, saved_call = g_dl_call_ordinal;
  g_dl_addr = addr; g_dl_draw_ordinal = 0; g_dl_call_ordinal = g_state_deltas ? 0 : g_dl_calls[addr]++;
  size_t used = 0;
  while (used < size) {
    size_t n = parse_command(p + used, size - used);
    if (!n) break;
    used += n;
  }
  g_dl_addr = saved_addr; g_dl_draw_ordinal = saved_draw; g_dl_call_ordinal = saved_call;
}

// Parses one command at `d`; returns its length, or 0 when `len` is short (g_need then holds the
// total length the command will have, so the FIFO writer can skip parsing until it has arrived).
size_t parse_command(const uint8_t* d, size_t len) {
  uint8_t op = d[0];
  if (op == 0x00) return 1;
  if (op == 0x08) { if (len < 6) return incomplete(6); g_cp.reg[d[1]] = be32(d + 2); ++g_cp_version; return 6; }
  if (op == 0x10) {
    if (len < 5) return incomplete(5);
    uint32_t count = (be16(d + 1) & 0xF) + 1, address = be16(d + 3);
    size_t need = 5 + count * 4;
    if (len < need) return incomplete(need);
    xf_load(address, count, d + 5);
    return need;
  }
  if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
    if (len < 5) return incomplete(5);
    xf_indexed_load(op, be32(d + 1));
    return 5;
  }
  if (op == 0x40) {
    if (len < 9) return incomplete(9);
    run_display_list(be32(d + 1), be32(d + 5));
    return 9;
  }
  if (op == 0x48) return 1;
  if (op == 0x61) { if (len < 5) return incomplete(5); bp_write(be32(d + 1)); return 5; }
  if (op >= 0x80 && op < 0xC0) {
    if (len < 3) return incomplete(3);
    uint32_t fmt = op & 7, count = be16(d + 1);
    const VertexDesc& desc = build_desc(fmt);
    size_t need = 3 + (size_t)count * desc.size;
    if (len < need) return incomplete(need);
    if (count) {
      uint32_t first = (uint32_t)g_frame.vertices.size();
      uint32_t components;
      if (g_raw_vertices) { capture_raw_vertices(desc, fmt, d + 3, count); components = components_of(desc); }
      else components = decode_vertices(desc, d + 3, count, fmt);
      record_draw(op & 0xF8, first, count, components);
    }
    return need;
  }
  static int reported = 0;
  if (reported++ < 10) host::log("gx: unknown opcode %02X", op);
  return 1;
}

}  // namespace

void mark_discontinuity() { g_discontinuity = true; }

void init(Backend* backend) {
  g_backend = backend;
  g_raw_vertices = backend && backend->wants_raw_vertices();
  g_state_deltas = backend && backend->wants_state_deltas();
  std::memset(&g_bp, 0, sizeof g_bp);
  std::memset(&g_cp, 0, sizeof g_cp);
  std::memset(&g_xf, 0, sizeof g_xf);
  std::memset(g_tmem, 0, sizeof g_tmem);
  g_frame.clear();
  g_buf_len = 0; g_buf_pos = 0; g_parse_need = 0; ++g_cp_version;
  if (g_buf.size() < (1u << 20)) g_buf.resize(1u << 20);
  g_fifo_data = g_buf.data(); g_fifo_cap = g_buf.size();
}

void fifo_parse_pending() {
  // Parses every complete command in the buffer; the remainder (an incomplete command) stays, and
  // g_fifo_need says how many bytes it is waiting for.
  g_parse_need = 0;
  while (g_buf_pos < g_buf_len) {
    size_t n = parse_command(g_buf.data() + g_buf_pos, g_buf_len - g_buf_pos);
    if (!n) break;               // parse_command recorded how many bytes it needs
    g_buf_pos += n;
    g_parse_need = 0;            // a display list inside the command may have left a stale need
    ++g_commands;
  }
  // Drained: the next command is at least a BP write (5 bytes; register writes, draws and display
  // list calls are all longer, and the 1-byte NOPs carry no state), so waking the parser for every
  // opcode byte or partial word is wasted work. A short pending tail is parsed by the next store.
  if (g_buf_pos == g_buf_len) { g_buf_len = 0; g_buf_pos = 0; g_parse_need = 5; }
  else if (g_buf_pos >= 1 << 16) { std::memmove(g_buf.data(), g_buf.data() + g_buf_pos, g_buf_len - g_buf_pos); g_buf_len -= g_buf_pos; g_buf_pos = 0; }
}
void fifo_grow() {
  g_buf.resize(std::max<size_t>(g_buf.size() * 2, 1u << 20));
  g_fifo_data = g_buf.data(); g_fifo_cap = g_buf.size();
}
void fifo_store32(uint32_t be) {   // the word is already in big-endian byte order
  if (__builtin_expect(g_fifo_len + 8 > g_fifo_cap, 0)) fifo_grow();
  std::memcpy(g_fifo_data + g_fifo_len, &be, 4);
  g_fifo_len += 4;
  if (g_fifo_len - g_fifo_pos >= g_fifo_need) fifo_parse_pending();
}
// Bytes and halfwords to the pipe (command opcodes, vertex counts, indices) take the same direct path
// as words; they used to go through the MMIO dispatch and write_fifo's byte loop.
void fifo_store8(uint32_t v) {
  if (__builtin_expect(g_fifo_len + 8 > g_fifo_cap, 0)) fifo_grow();
  g_fifo_data[g_fifo_len++] = uint8_t(v);
  if (g_fifo_len - g_fifo_pos >= g_fifo_need) fifo_parse_pending();
}
void fifo_store16(uint32_t v) {   // v in host order; stored big-endian
  if (__builtin_expect(g_fifo_len + 8 > g_fifo_cap, 0)) fifo_grow();
  g_fifo_data[g_fifo_len] = uint8_t(v >> 8); g_fifo_data[g_fifo_len + 1] = uint8_t(v);
  g_fifo_len += 2;
  if (g_fifo_len - g_fifo_pos >= g_fifo_need) fifo_parse_pending();
}
void write_fifo(uint32_t value, int bytes) {
  // The game writes the FIFO a word (or less) at a time, tens of thousands of times a frame:
  // the buffer keeps its storage and only a length moves; growth is rare and geometric.
  const size_t at = g_buf_len;
  if (at + 8 > g_buf.size()) fifo_grow();
  uint8_t* out = g_buf.data() + at;
  for (int i = 0; i < bytes; ++i) out[i] = (uint8_t)(value >> (8 * (bytes - 1 - i)));
  g_buf_len = at + (size_t)bytes;
  if (g_buf_len - g_buf_pos < g_parse_need) return;   // the pending command is still incomplete
  fifo_parse_pending();
}

void stats(uint64_t* commands, uint64_t* draws, uint64_t* vertices, uint32_t* efb_copies) {
  if (commands) *commands = g_commands;
  if (draws) *draws = g_draws;
  if (vertices) *vertices = g_vertices;
  if (efb_copies) *efb_copies = g_efb_copies;
}

const uint8_t* tmem() { return g_tmem; }

void apply_state(DrawState& into, const DrawCall& from) { apply_state_impl(into, from); }

}  // namespace gx
