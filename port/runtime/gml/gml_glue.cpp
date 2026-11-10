// See gml_glue.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gml_glue.h"
#include "gx_core.h"
#include "host.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
uint8_t* gml_ram = nullptr;
uint32_t* gml_regs = nullptr;
double* gml_fregs = nullptr;
uint8_t* gml_cr = nullptr;
uint32_t gml_ld_slow(uint32_t addr, uint32_t bytes) { return ppc::ld_slow(*host::cpu, addr, int(bytes)); }
uint64_t gml_ld64_slow(uint32_t addr) { return ppc::ld64_slow(*host::cpu, addr); }
void gml_st_slow(uint32_t addr, uint32_t value, uint32_t bytes) { ppc::st_slow(*host::cpu, addr, value, int(bytes)); }
void gml_st64_slow(uint32_t addr, uint64_t value) { ppc::st64_slow(*host::cpu, addr, value); }
void gml_memcpy_slow(uint32_t d, uint32_t s, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ppc::st8(*host::cpu, gml_ram, d + i, ppc::ld8(*host::cpu, gml_ram, s + i)); }
void gml_memset_slow(uint32_t d, uint32_t v, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ppc::st8(*host::cpu, gml_ram, d + i, v); }
// A call from native code into the translated game (PowerPC EABI: the lowered code has set r3..
// and f1..). Native code that the game calls back into re-enters through the HLE wrappers.
void gml_guest_call(uint32_t addr) {
  ppc::Context& c = *host::cpu;
  uint8_t* m = gml_ram;
  ppc::call(c, m, addr);
  gml_enter(c, m);
}
void gml_panic(void) { ppc::fatal(*host::cpu, "native decomp code reached an assertion or panic", host::cpu->lr); }
}

namespace {
// HSD_TExpSetReg (texp.c) builds TEV colours in an uninitialised GXColor reg[8]: a register set
// only for RGB keeps the stack's leftover alpha, one set only for alpha keeps leftover RGB. On the
// GameCube those channels are whatever the stack held (and the TEV never reads them). Native frames
// leave different leftovers, so the values of the TEV colour registers (BP 0xE0-0xE7) are masked;
// which registers are written, and in what order, is still compared.
std::vector<uint8_t> mask_tev_alpha(std::vector<uint8_t> p) {
  for (size_t i = 0; i + 5 <= p.size(); ++i)
    if (p[i] == 0x61 && p[i + 1] >= 0xE0 && p[i + 1] <= 0xE7) {
      p[i + 2] &= 0x80; p[i + 3] = 0; p[i + 4] = 0;   // keep only bit 23 (konst flag)
      i += 4;
    }
  return p;
}
}  // namespace

// gml_verify=1 (sampled): the first 32 calls of every native entry and then one in 512 run natively,
// all of guest RAM and the pipe are recorded and rewound, the original translation runs, and the two
// results are compared outside the stack area (both write frames there, differently).
bool gml::verify_on() { static const bool on = [] { const char* v = std::getenv("MELEE_GML_VERIFY"); return v && v[0] == '1'; }(); return on; }
void gml::verify_run(ppc::Context& c, uint8_t* m, const char* name, void (*orig)(ppc::Context&, uint8_t*), void (*thunk)(void*), void* native) {
  static int depth = 0;
  static std::map<std::string, uint64_t> counts;
  static uint64_t checked = 0, mismatches = 0;
  static std::vector<uint8_t> before, after_native;
  uint64_t& n = counts[name];
  // A frame window (gml_verify_frames=A:B) checks every call in it; otherwise a sample.
  static const auto window = [] {
    std::pair<uint32_t, uint32_t> w{0, 0};
    if (const char* v = std::getenv("MELEE_GML_VERIFY_FRAMES")) std::sscanf(v, "%u:%u", &w.first, &w.second);
    return w;
  }();
  ++n;
  const uint32_t frame = host::retrace_count();
  const bool in_window = window.second && frame >= window.first && frame <= window.second;
  if (depth || (window.second ? !in_window : (n > 32 && n % 512))) { thunk(native); return; }
  struct Guard { Guard() { ++depth; } ~Guard() { --depth; } } guard;
  const uint32_t sp = c.r[1] & 0x01FFFFFFu;
  before.assign(m, m + ppc::RAM_SIZE);
  const ppc::Context saved = c;
  const auto mark = gx::verify_mark();
  thunk(native);
  const uint32_t r3n = c.r[3]; const uint64_t f1n = c.f[1].u0;
  std::vector<uint8_t> pipe_native, pipe_orig;
  if (!gx::verify_take(mark, pipe_native)) { static int noted = 0; if (noted++ < 4) host::log("gml-verify %s: pipe handed off, not compared", name); return; }
  after_native.assign(m, m + ppc::RAM_SIZE);
  std::memcpy(m, before.data(), ppc::RAM_SIZE);
  c = saved;
  orig(c, m);
  if (gx::verify_take(mark, pipe_orig)) gx::verify_put(pipe_orig);
  ++checked;
  // The stack area: up to 64 KiB below the caller's stack pointer, plus the caller's LR save word
  // (sp+4), which a non-leaf PowerPC callee writes and native code does not.
  const uint32_t lo = sp > 0x10000 ? sp - 0x10000 : 0, hi = sp + 8;
  uint32_t first = UINT32_MAX, count = 0;
  // lbaudio_ax.c's buffers: audio callbacks run inside the translation (its loops poll for events)
  // and not inside native code, so they change between the two runs; not part of what is compared.
  constexpr uint32_t audio_lo = 0x00433700, audio_hi = 0x00452C80;
  for (uint32_t a = 0; a < ppc::RAM_SIZE; a += 64) {
    if (a + 64 > lo && a < hi) continue;
    if (a + 64 > audio_lo && a < audio_hi) continue;
    if (std::memcmp(m + a, after_native.data() + a, 64)) {
      for (uint32_t k = a; k < a + 64; ++k) if (m[k] != after_native[k]) { if (first == UINT32_MAX) first = k; ++count; }
    }
  }
  const bool ret_differs = r3n != c.r[3] && f1n != c.f[1].u0;
  const bool pipe_same = mask_tev_alpha(pipe_native) == mask_tev_alpha(pipe_orig);
  if (count || !pipe_same) {
    if (++mismatches <= 40) {
      char bytes[64] = "";
      if (first != UINT32_MAX) {
        const uint32_t w = first & ~3u;
        std::snprintf(bytes, sizeof bytes, "%02X%02X%02X%02X / %02X%02X%02X%02X", after_native[w], after_native[w + 1], after_native[w + 2],
                      after_native[w + 3], m[w], m[w + 1], m[w + 2], m[w + 3]);
      }
      const auto mn = mask_tev_alpha(pipe_native), mo = mask_tev_alpha(pipe_orig);
      if (mn != mo && mn.size() == mo.size()) {
        size_t k = 0; while (mn[k] == mo[k]) ++k;
        const size_t from = k >= 12 ? k - 12 : 0;
        char a[100] = "", b2[100] = ""; int na = 0, nb = 0;
        for (size_t i = from; i < from + 24 && i < pipe_native.size(); ++i) {
          na += std::snprintf(a + na, sizeof a - na, "%02X", pipe_native[i]); nb += std::snprintf(b2 + nb, sizeof b2 - nb, "%02X", pipe_orig[i]);
        }
        host::log("gml-verify %s pipe differs at byte %zu of %zu: native %s / original %s", name, k, pipe_native.size(), a, b2);
      }
      host::log("gml-verify %s MISMATCH (frame %u, call %llu, r3=%08X r4=%08X lr=%08X): %u bytes differ, first at %08X (%s), pipe %s (%zu/%zu)%s", name, frame,
                (unsigned long long)n, saved.r[3], saved.r[4], saved.lr, count, first == UINT32_MAX ? 0u : first | 0x80000000u, bytes,
                pipe_native == pipe_orig ? "same" : "DIFFERS", pipe_native.size(), pipe_orig.size(), ret_differs ? ", return differs" : "");
    }
  }
  if ((checked & (checked - 1)) == 0 && checked >= 64)
    host::log("gml-verify: %llu sampled calls compared, %llu mismatches", (unsigned long long)checked, (unsigned long long)mismatches);
}
