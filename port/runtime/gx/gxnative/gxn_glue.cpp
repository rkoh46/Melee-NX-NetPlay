// Host side of the native GX (tools/generate_gx_native.py): pipe writes, the guest callbacks, the
// matrix-to-pipe copies, and the call-by-call comparison against the original translation.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gxn_glue.h"
#include "gxn_glue_host.h"
#include "gx_core.h"
#include "hle.h"
#include "host.h"
#include <cstdlib>
#include <vector>

extern "C" uint8_t* gxn_gx = nullptr;
namespace gxn {
ppc::Context* g_ctx = nullptr;
uint8_t* g_m = nullptr;
bool verify_on() { static const bool on = [] { const char* v = std::getenv("MELEE_GXN_VERIFY"); return v && v[0] == '1'; }(); return on; }
}  // namespace gxn

namespace {
inline uint32_t guest_of(const void* p) { return 0x80000000u | uint32_t(static_cast<const uint8_t*>(p) - gxn::g_m); }
inline uint32_t gx_field(uint32_t offset) { uint32_t v; std::memcpy(&v, gxn_gx + offset, 4); return __builtin_bswap32(v); }
// The original matrix copies (hle_mtx.cpp: exact paired-single semantics) read r3 = matrix, r4 = pipe.
void write_matrix(void (*routine)(ppc::Context&, uint8_t*), const void* guest_mtx) {
  ppc::Context& c = *gxn::g_ctx;
  c.r[3] = uint32_t(reinterpret_cast<uintptr_t>(guest_mtx)); c.r[4] = 0xCC008000u;
  routine(c, gxn::g_m);
}
}  // namespace

extern "C" {
void gxn_write_u8(unsigned char v) { gx::fifo_store8(v); }
void gxn_write_u16(unsigned short v) { gx::fifo_store16(v); }
void gxn_write_u32(unsigned int v) { gx::fifo_store32(__builtin_bswap32(v)); }
void gxn_write_f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); gx::fifo_store32(__builtin_bswap32(v)); }
void gxn___GXSetRange(float, float) {}   // GXStubs.c: empty in the SDK too
// __GXData.texRegionCallback (+0x410) and tlutRegionCallback (+0x414) are guest functions.
void* gxn_tex_region_callback(void* obj, int id) {
  ppc::Context& c = *gxn::g_ctx;
  c.r[3] = guest_of(obj); c.r[4] = uint32_t(id);
  ppc::call(c, gxn::g_m, gx_field(0x410));
  return gxn_host(gxn::g_m, c.r[3]);
}
void* gxn_tlut_region_callback(unsigned long idx) {
  ppc::Context& c = *gxn::g_ctx;
  c.r[3] = uint32_t(idx);
  ppc::call(c, gxn::g_m, gx_field(0x414));
  return gxn_host(gxn::g_m, c.r[3]);
}
void gxn_WriteMTXPS4x3(const void* g) { write_matrix(hle::WriteMTXPS4x3, g); }
void gxn_WriteMTXPS3x3from3x4(const void* g) { write_matrix(hle::WriteMTXPS3x3from3x4, g); }
// Only GXLoadNrmMtxImm3x3 uses it, which Melee does not contain (so it is never hooked or called).
void gxn_WriteMTXPS3x3(const void* g) { ppc::fatal(*gxn::g_ctx, "native GX: WriteMTXPS3x3 is not provided", uint32_t(reinterpret_cast<uintptr_t>(g))); }
void gxn_WriteMTXPS4x2(const void* g) { write_matrix(hle::WriteMTXPS4x2, g); }
}

// gxn_verify=1: every hooked call runs natively, its effects are recorded and undone, then the
// original translation runs and its effects stand. Compared: the pipe bytes, __GXData, 64 bytes at
// every argument register that points into RAM (the objects the call fills or reads), r3 and f1.
void gxn::verify_run(ppc::Context& c, uint8_t* m, const char* name, void (*orig)(ppc::Context&, uint8_t*),
                     void (*thunk)(void*), void* native) {
  static uint64_t calls = 0, mismatches = 0;
  static int depth = 0;
  // A call made from inside another (a region callback runs guest code that calls GX) runs natively
  // unchecked: nested marks would hand the pipe chunk off under the outer comparison.
  if (depth) { thunk(native); return; }
  struct DepthGuard { DepthGuard() { ++depth; } ~DepthGuard() { --depth; } } guard;
  struct Region { uint32_t off, len; };
  std::vector<Region> regions{{uint32_t(gxn_gx - m), 0x4F4}};
  for (int r = 3; r <= 10; ++r) {
    const uint32_t a = c.r[r];
    if ((a & 0xFE000000u) == 0x80000000u && (a & 0x01FFFFFFu) + 64 <= ppc::RAM_SIZE) regions.push_back({a & 0x01FFFFFFu, 64});
  }
  std::vector<uint8_t> before, after_native, after_orig;
  for (const auto& r : regions) before.insert(before.end(), m + r.off, m + r.off + r.len);
  const ppc::Context saved = c;
  const auto mark = gx::verify_mark();
  thunk(native);
  const uint32_t r3n = c.r[3]; const uint64_t f1n = c.f[1].u0;
  std::vector<uint8_t> pipe_native, pipe_orig;
  gx::verify_take(mark, pipe_native);
  for (const auto& r : regions) after_native.insert(after_native.end(), m + r.off, m + r.off + r.len);
  size_t k = 0;
  for (const auto& r : regions) { std::memcpy(m + r.off, before.data() + k, r.len); k += r.len; }
  c = saved;
  orig(c, m);
  gx::verify_take(mark, pipe_orig);
  for (const auto& r : regions) after_orig.insert(after_orig.end(), m + r.off, m + r.off + r.len);
  gx::verify_put(pipe_orig);   // the original's pipe bytes stand
  ++calls;
  const bool same_pipe = pipe_native == pipe_orig, same_mem = after_native == after_orig;
  (void)r3n; (void)f1n;
  if (!same_pipe || !same_mem) {
    if (++mismatches <= 24) {
      size_t at = 0; while (at < after_native.size() && after_native[at] == after_orig[at]) ++at;
      // Which region and offset, and the bytes there (native / original).
      size_t region = 0, base = 0; while (region + 1 < regions.size() && base + regions[region].len <= at) base += regions[region++].len;
      char mem[160] = "", pipe[200] = ""; int n = 0;
      if (!same_mem) {
        const size_t from = at & ~size_t(3);
        for (size_t i = from; i < from + 8 && i < after_native.size(); ++i) n += std::snprintf(mem + n, sizeof mem - n, "%02X", after_native[i]);
        n += std::snprintf(mem + n, sizeof mem - n, " / ");
        for (size_t i = from; i < from + 8 && i < after_orig.size(); ++i) n += std::snprintf(mem + n, sizeof mem - n, "%02X", after_orig[i]);
      }
      n = 0;
      if (!same_pipe) {
        for (size_t i = 0; i < pipe_native.size() && i < 16; ++i) n += std::snprintf(pipe + n, sizeof pipe - n, "%02X", pipe_native[i]);
        n += std::snprintf(pipe + n, sizeof pipe - n, " / ");
        for (size_t i = 0; i < pipe_orig.size() && i < 16; ++i) n += std::snprintf(pipe + n, sizeof pipe - n, "%02X", pipe_orig[i]);
      }
      host::log("gxn-verify %s MISMATCH call %llu: pipe %s %s | memory %s region %zu (+%zu) %s",
                name, (unsigned long long)calls, same_pipe ? "same" : "DIFFERS", pipe,
                same_mem ? "same" : "DIFFERS", region, at - base, mem);
    }
  }
  if ((calls & (calls - 1)) == 0 && calls >= 1024)
    host::log("gxn-verify: %llu calls, %llu mismatches", (unsigned long long)calls, (unsigned long long)mismatches);
}
