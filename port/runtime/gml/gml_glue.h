// Host side of natively compiled decomp C (docs/NATIVE_C.md, tools/gml): the guest pointers the
// lowered code reads, entry from guest registers, and the sampled comparison with the translation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ppc.h"
#include <cstdint>

extern "C" {
extern uint8_t* gml_ram;
extern uint32_t* gml_regs;
extern double* gml_fregs;
extern uint8_t* gml_cr;
}
namespace gml {
extern bool g_verify;         // launcher.ini gml_verify=1 / gml_verify_frames (set by main_switch)
inline bool verify_on() { return g_verify; }
void verify_run(ppc::Context& c, uint8_t* m, const char* name, void (*orig)(ppc::Context&, uint8_t*), void (*thunk)(void*), void* native);
}
inline void gml_enter(ppc::Context& c, uint8_t* m) {
  gml_ram = m; gml_regs = c.r; gml_fregs = &c.f[0].ps0; gml_cr = c.cr;
}
template <class F>
inline void gml_verified(ppc::Context& c, uint8_t* m, const char* name, void (*orig)(ppc::Context&, uint8_t*), F&& native) {
  if (__builtin_expect(!gml::verify_on(), 1)) { native(); return; }
  gml::verify_run(c, m, name, orig, [](void* p) { (*static_cast<F*>(p))(); }, &native);
}
#define GML_VERIFIED(name, orig, ...) gml_verified(c, m, #name, orig, __VA_ARGS__)
