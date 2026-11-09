// Host implementations of the SDK's hottest paired-single matrix routines. Melee's HSD scene
// graph calls PSMTXConcat for every joint every frame and GXLoadPosMtxImm (WriteMTXPS4x3) for
// every drawn object; translated, the two were a tenth of the simulation thread on the console.
//
// Bit-exactness: each routine performs exactly the operations of the original Gekko assembly
// (extern/dolphin/src/dolphin/mtx/mtx.c, gx/GXTransform.c) through the same ppc:: numeric helpers
// the recompiled code uses (psq_l/psq_st conversions, ps_muls0/ps_madds* rounding), in the same
// order per lane, so the results match the translation to the bit. Only the register traffic,
// stack spills and per-word memory-access dispatch are gone.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hle.h"
#include "gx_core.h"

namespace {
// psq_l / psq_st with GQR type 0 (floats): what every matrix routine uses.
inline double ps_load(uint8_t* m, ppc::Context& c, uint32_t ea) { return ppc::float_bits_to_double(ppc::ld32(c, m, ea)); }
inline void ps_store(uint8_t* m, ppc::Context& c, uint32_t ea, double v) { ppc::st32(c, m, ea, ppc::double_to_float_bits_ftz(v)); }
// ps_muls0 / ps_madds0 / ps_madds1 on one lane: multiplier rounded to 25 bits, single-precision result.
inline double ps_mul(double a, double k) { return ppc::fs(ppc::fmul(a, k)); }
inline double ps_madd(double a, double k, double b) { return ppc::fs(ppc::fmadds(a, k, b)); }
}  // namespace

// void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab): ab = a * b (3x4 affine matrices).
HLE(PSMTXConcat) {
  const uint32_t a = ARG0, b = ARG1, ab = ARG2;
  double A[3][4], B[3][4], R[3][4];
  for (int r = 0; r < 3; ++r)
    for (int col = 0; col < 4; ++col) { A[r][col] = ps_load(m, c, a + r * 16 + col * 4); B[r][col] = ps_load(m, c, b + r * 16 + col * 4); }
  const double unit01[2] = {0.0, 1.0};   // Unit01: the pair that adds a[r][3] to the translation lane
  for (int r = 0; r < 3; ++r) {
    const double k0 = ppc::f25(A[r][0]), k1 = ppc::f25(A[r][1]), k2 = ppc::f25(A[r][2]), k3 = ppc::f25(A[r][3]);
    for (int lane = 0; lane < 2; ++lane) {
      // Columns 0,1: ps_muls0(B0, a0) -> ps_madds1(B1, a1) -> ps_madds0(B2, a2).
      double t = ps_mul(B[0][lane], k0);
      t = ps_madd(B[1][lane], k1, t);
      t = ps_madd(B[2][lane], k2, t);
      R[r][lane] = t;
      // Columns 2,3: the same, then ps_madds1(Unit01, a3) adds a3 to column 3 only.
      double u = ps_mul(B[0][2 + lane], k0);
      u = ps_madd(B[1][2 + lane], k1, u);
      u = ps_madd(B[2][2 + lane], k2, u);
      u = ps_madd(unit01[lane], k3, u);
      R[r][2 + lane] = u;
    }
  }
  for (int r = 0; r < 3; ++r) for (int col = 0; col < 4; ++col) ps_store(m, c, ab + r * 16 + col * 4, R[r][col]);
}

namespace {
// The GX matrix writers stream a matrix into the write-gather pipe: psq_st pairs (type 0, flush
// to zero on store) and stfs singles for odd elements. `dest` is always the pipe, so the words go
// straight to the FIFO capture after the same load/store conversions the translation performs.
inline void fifo_pair(uint8_t* m, ppc::Context& c, uint32_t src) {
  gx::write_fifo(ppc::double_to_float_bits_ftz(ps_load(m, c, src)), 4);
  gx::write_fifo(ppc::double_to_float_bits_ftz(ps_load(m, c, src + 4)), 4);
}
inline void fifo_single(uint8_t* m, ppc::Context& c, uint32_t src) {
  gx::write_fifo(ppc::double_to_float_bits(ps_load(m, c, src)), 4);
}
}  // namespace

// static void WriteMTXPS4x3(f32 mtx[3][4], volatile f32* dest): six pairs.
HLE(WriteMTXPS4x3) {
  const uint32_t mtx = ARG0;
  for (uint32_t off = 0; off < 48; off += 8) fifo_pair(m, c, mtx + off);
}
// static void WriteMTXPS3x3from3x4(f32 mtx[3][4], volatile f32* dest): per row a pair then a single.
HLE(WriteMTXPS3x3from3x4) {
  const uint32_t mtx = ARG0;
  for (uint32_t row = 0; row < 3; ++row) { fifo_pair(m, c, mtx + row * 16); fifo_single(m, c, mtx + row * 16 + 8); }
}
// static void WriteMTXPS4x2(f32 mtx[2][4], volatile f32* dest): four pairs.
HLE(WriteMTXPS4x2) {
  const uint32_t mtx = ARG0;
  for (uint32_t off = 0; off < 32; off += 8) fifo_pair(m, c, mtx + off);
}
