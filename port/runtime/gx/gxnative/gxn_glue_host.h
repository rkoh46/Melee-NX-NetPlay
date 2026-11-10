// C++ side of the native GX (tools/generate_gx_native.py): what the generated wrappers in
// gxn_hle.cpp use to call the natively compiled decomp GX functions from guest registers.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ppc.h"
#include <cstdint>
#include <cstring>

// GXColor / GXColorS10 by value: the guest passes a pointer to a copy; the native function takes the
// raw (big-endian) bytes in a register, which is what these carry.
struct GxnBytes4 { uint8_t b[4]; };
struct GxnBytes8 { uint8_t b[8]; };

extern "C" uint8_t* gxn_gx;   // __GXData in guest RAM (the C code's `gx`)
namespace gxn {
extern ppc::Context* g_ctx;   // the guest context of the outermost native GX call (for callbacks)
extern uint8_t* g_m;
extern bool g_verify;         // launcher.ini gxn_verify=1 (set by main_switch)
inline bool verify_on() { return g_verify; }   // one load on every native GX call
void verify_run(ppc::Context& c, uint8_t* m, const char* name, void (*orig)(ppc::Context&, uint8_t*),
                void (*thunk)(void*), void* native);
}  // namespace gxn

inline void* gxn_host(uint8_t* m, uint32_t guest) { return m + (guest & 0x01FFFFFFu); }
inline void gxn_enter(ppc::Context& c, uint8_t* m) {
  // `gx` (an SDA variable, r13 - 23288 in every translated GX function) points at the state block.
  if (__builtin_expect(!gxn_gx, 0)) gxn_gx = m + (ppc::ld32(c, m, c.r[13] - 23288u) & 0x01FFFFFFu);
  gxn::g_ctx = &c; gxn::g_m = m;
}
inline void gxn_read_floats(uint8_t* m, uint32_t guest, float* out, int n) {
  for (int i = 0; i < n; ++i) { const uint32_t w = ppc::ld32(*gxn::g_ctx, m, guest + 4u * uint32_t(i)); std::memcpy(out + i, &w, 4); }
}
template <class F>
inline void gxn_verified(ppc::Context& c, uint8_t* m, const char* name, void (*orig)(ppc::Context&, uint8_t*), F&& native) {
  if (__builtin_expect(!gxn::verify_on(), 1)) { native(); return; }
  gxn::verify_run(c, m, name, orig, [](void* p) { (*static_cast<F*>(p))(); }, &native);
}
#define GXN_VERIFIED(name, orig, ...) gxn_verified(c, m, #name, orig, __VA_ARGS__)
