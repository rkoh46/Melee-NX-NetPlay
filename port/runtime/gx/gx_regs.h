// GX register images (BP / CP / XF) as written by the recompiled GX SDK through the FIFO.
// Layouts follow Dolphin's VideoCommon (BPMemory.h, CPMemory.h, XFMemory.h), GPL-2.0-or-later.
#pragma once
#include <cstdint>
#include <cstring>

namespace gx {

inline uint32_t bits(uint32_t v, int lo, int count) { return (v >> lo) & ((1u << count) - 1); }
inline int32_t sbits(uint32_t v, int lo, int count) {
  uint32_t u = bits(v, lo, count);
  return (int32_t)(u << (32 - count)) >> (32 - count);
}

// ---------------- BP ----------------
enum : uint8_t {
  BP_GENMODE = 0x00, BP_IND_MTXA = 0x06, BP_IND_IMASK = 0x0F, BP_IND_CMD = 0x10,
  BP_SCISSORTL = 0x20, BP_SCISSORBR = 0x21, BP_LINEPTWIDTH = 0x22, BP_RAS1_SS0 = 0x25, BP_RAS1_SS1 = 0x26,
  BP_IREF = 0x27, BP_TREF = 0x28, BP_SU_SSIZE = 0x30, BP_ZMODE = 0x40, BP_BLENDMODE = 0x41,
  BP_CONSTANTALPHA = 0x42, BP_ZCOMPARE = 0x43, BP_FIELDMASK = 0x44, BP_SETDRAWDONE = 0x45,
  BP_PE_TOKEN_ID = 0x47, BP_PE_TOKEN_INT_ID = 0x48, BP_EFB_TL = 0x49, BP_EFB_BR = 0x4A, BP_EFB_ADDR = 0x4B,
  BP_MIPMAP_STRIDE = 0x4D, BP_COPYYSCALE = 0x4E, BP_CLEAR_AR = 0x4F, BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51,
  BP_TRIGGER_EFB_COPY = 0x52, BP_COPYFILTER0 = 0x53, BP_SCISSOROFFSET = 0x59,
  BP_PRELOAD_ADDR = 0x60, BP_PRELOAD_MODE = 0x63, BP_LOADTLUT0 = 0x64, BP_LOADTLUT1 = 0x65,
  BP_TEXINVALIDATE = 0x66, BP_FIELDMODE = 0x68,
  BP_TX_SETMODE0 = 0x80, BP_TX_SETMODE1 = 0x84, BP_TX_SETIMAGE0 = 0x88, BP_TX_SETIMAGE1 = 0x8C,
  BP_TX_SETIMAGE2 = 0x90, BP_TX_SETIMAGE3 = 0x94, BP_TX_SETTLUT = 0x98,
  BP_TEV_COLOR_ENV = 0xC0, BP_TEV_ALPHA_ENV = 0xC1, BP_TEV_COLOR_RA = 0xE0, BP_TEV_COLOR_BG = 0xE1,
  BP_FOGRANGE = 0xE8, BP_FOGPARAM0 = 0xEE, BP_FOGBMAGNITUDE = 0xEF, BP_FOGBEXPONENT = 0xF0,
  BP_FOGPARAM3 = 0xF1, BP_FOGCOLOR = 0xF2, BP_ALPHACOMPARE = 0xF3, BP_BIAS = 0xF4, BP_ZTEX2 = 0xF5,
  BP_TEV_KSEL = 0xF6, BP_BP_MASK = 0xFE,
};

struct BPMemory {
  uint32_t reg[256];  // raw 24-bit values, index = register id

  // GenMode
  uint32_t numtexgens() const { return bits(reg[0], 0, 4); }
  uint32_t numcolchans() const { return bits(reg[0], 4, 3); }
  uint32_t numtevstages() const { return bits(reg[0], 10, 4); }
  uint32_t cullmode() const { return bits(reg[0], 14, 2); }
  uint32_t numindstages() const { return bits(reg[0], 16, 3); }
  uint32_t zfreeze() const { return bits(reg[0], 19, 1); }
  // TEV
  uint32_t tev_color(int stage) const { return reg[BP_TEV_COLOR_ENV + 2 * stage]; }
  uint32_t tev_alpha(int stage) const { return reg[BP_TEV_ALPHA_ENV + 2 * stage]; }
  uint32_t tref(int i) const { return reg[BP_TREF + i]; }
  uint32_t tevind(int stage) const { return reg[BP_IND_CMD + stage]; }
  uint32_t ksel(int i) const { return reg[BP_TEV_KSEL + i]; }
  uint32_t texscale(int i) const { return reg[BP_RAS1_SS0 + i]; }
  uint32_t iref() const { return reg[BP_IREF]; }
  // tevorders (TREF)
  int order_texmap(int stage) const { return bits(tref(stage / 2), (stage & 1) ? 12 : 0, 3); }
  int order_texcoord(int stage) const { return bits(tref(stage / 2), (stage & 1) ? 15 : 3, 3); }
  int order_enable(int stage) const { return bits(tref(stage / 2), (stage & 1) ? 18 : 6, 1); }
  int order_colorchan(int stage) const { return bits(tref(stage / 2), (stage & 1) ? 19 : 7, 3); }
  // ksel
  int ksel_swap1(int i) const { return bits(ksel(i), 0, 2); }
  int ksel_swap2(int i) const { return bits(ksel(i), 2, 2); }
  int ksel_kc(int stage) const { return bits(ksel(stage / 2), (stage & 1) ? 14 : 4, 5); }
  int ksel_ka(int stage) const { return bits(ksel(stage / 2), (stage & 1) ? 19 : 9, 5); }
  // z / blend / alpha
  uint32_t zmode() const { return reg[BP_ZMODE]; }
  uint32_t blendmode() const { return reg[BP_BLENDMODE]; }
  uint32_t dstalpha() const { return reg[BP_CONSTANTALPHA]; }
  uint32_t zcontrol() const { return reg[BP_ZCOMPARE]; }
  uint32_t alpha_test() const { return reg[BP_ALPHACOMPARE]; }
  uint32_t ztex1() const { return reg[BP_BIAS]; }
  uint32_t ztex2() const { return reg[BP_ZTEX2]; }
  // fog
  uint32_t fogparam0() const { return reg[BP_FOGPARAM0]; }
  uint32_t fog_bmag() const { return reg[BP_FOGBMAGNITUDE]; }
  uint32_t fog_bshift() const { return reg[BP_FOGBEXPONENT]; }
  uint32_t fogparam3() const { return reg[BP_FOGPARAM3]; }
  uint32_t fogcolor() const { return reg[BP_FOGCOLOR]; }
  uint32_t fogrange(int i) const { return reg[BP_FOGRANGE + i]; }
  // texture units: unit i in 0..7 -> bank i/4, slot i%4
  uint32_t texmode0(int i) const { return reg[(i < 4 ? BP_TX_SETMODE0 : 0xA0) + (i & 3)]; }
  uint32_t texmode1(int i) const { return reg[(i < 4 ? BP_TX_SETMODE1 : 0xA4) + (i & 3)]; }
  uint32_t teximage0(int i) const { return reg[(i < 4 ? BP_TX_SETIMAGE0 : 0xA8) + (i & 3)]; }
  uint32_t teximage1(int i) const { return reg[(i < 4 ? BP_TX_SETIMAGE1 : 0xAC) + (i & 3)]; }
  uint32_t teximage2(int i) const { return reg[(i < 4 ? BP_TX_SETIMAGE2 : 0xB0) + (i & 3)]; }
  uint32_t teximage3(int i) const { return reg[(i < 4 ? BP_TX_SETIMAGE3 : 0xB4) + (i & 3)]; }
  uint32_t textlut(int i) const { return reg[(i < 4 ? BP_TX_SETTLUT : 0xB8) + (i & 3)]; }
  uint32_t texcoord_s(int i) const { return reg[BP_SU_SSIZE + 2 * i]; }
  uint32_t texcoord_t(int i) const { return reg[BP_SU_SSIZE + 2 * i + 1]; }
  // indirect matrices
  uint32_t indmtx(int m, int col) const { return reg[BP_IND_MTXA + 3 * m + col]; }
};

// ---------------- CP ----------------
struct CPMemory {
  uint32_t reg[256];
  uint32_t array_base(int i) const { return reg[0xA0 + i]; }
  uint32_t array_stride(int i) const { return reg[0xB0 + i]; }
  uint32_t matrix_index_a() const { return reg[0x30]; }
  uint32_t matrix_index_b() const { return reg[0x40]; }
  uint32_t vcd_lo() const { return reg[0x50]; }
  uint32_t vcd_hi() const { return reg[0x60]; }
  uint32_t vat_a(int f) const { return reg[0x70 + f]; }
  uint32_t vat_b(int f) const { return reg[0x80 + f]; }
  uint32_t vat_c(int f) const { return reg[0x90 + f]; }
};

// ---------------- XF ----------------
struct XFMemory {
  union {
    uint32_t raw[0x1058];
    struct {
      float posMatrices[256];     // 0x0000
      uint32_t unk0[768];
      float normalMatrices[96];   // 0x0400
      uint32_t unk1[160];
      float postMatrices[256];    // 0x0500
      struct { uint32_t useless[3]; uint8_t color[4]; float cosatt[3]; float distatt[3]; float dpos[3]; float ddir[3]; } lights[8];  // 0x0600
      uint32_t unk2[2432];
      uint32_t error, diag, state0, state1, xfClock, clipDisable, perf0, perf1;  // 0x1000
      uint32_t hostinfo;          // 0x1008
      uint32_t numChan;           // 0x1009
      uint32_t ambColor[2];       // 0x100a
      uint32_t matColor[2];       // 0x100c
      uint32_t color[2];          // 0x100e LitChannel
      uint32_t alpha[2];          // 0x1010
      uint32_t dualTexTrans;      // 0x1012
      uint32_t unk3[5];
      uint32_t matrixIndexA;      // 0x1018
      uint32_t matrixIndexB;      // 0x1019
      float viewport[6];          // 0x101a: wd, ht, zRange, xOrig, yOrig, farZ
      float projection[6];        // 0x1020
      uint32_t projectionType;    // 0x1026
      uint32_t unk8[24];
      uint32_t numTexGen;         // 0x103f
      uint32_t texMtxInfo[8];     // 0x1040
      uint32_t unk9[8];
      uint32_t postMtxInfo[8];    // 0x1050
    };
  };
  uint32_t num_color_chans() const { return numChan & 3; }
  uint32_t num_texgens() const { return numTexGen & 15; }
};

// LitChannel fields
inline uint32_t lit_matsource(uint32_t v) { return bits(v, 0, 1); }
inline uint32_t lit_enable(uint32_t v) { return bits(v, 1, 1); }
inline uint32_t lit_ambsource(uint32_t v) { return bits(v, 6, 1); }
inline uint32_t lit_diffusefunc(uint32_t v) { return bits(v, 7, 2); }
inline uint32_t lit_attnfunc(uint32_t v) { return bits(v, 9, 2); }
inline uint32_t lit_lightmask(uint32_t v) { return lit_enable(v) ? (bits(v, 2, 4) | (bits(v, 11, 4) << 4)) : 0; }
// TexMtxInfo fields
inline uint32_t tmi_projection(uint32_t v) { return bits(v, 1, 1); }
inline uint32_t tmi_inputform(uint32_t v) { return bits(v, 2, 1); }
inline uint32_t tmi_texgentype(uint32_t v) { return bits(v, 4, 3); }
inline uint32_t tmi_sourcerow(uint32_t v) { return bits(v, 7, 5); }
inline uint32_t tmi_embosssourceshift(uint32_t v) { return bits(v, 12, 3); }
inline uint32_t tmi_embosslightshift(uint32_t v) { return bits(v, 15, 3); }

}  // namespace gx
