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
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
// psq_l / psq_st with GQR type 0 (floats): what every matrix routine uses.
inline double ps_load(uint8_t* m, ppc::Context& c, uint32_t ea) { return ppc::float_bits_to_double(ppc::ld32(c, m, ea)); }
inline void ps_store(uint8_t* m, ppc::Context& c, uint32_t ea, double v) { ppc::st32(c, m, ea, ppc::double_to_float_bits_ftz(v)); }
// ps_muls0 / ps_madds0 / ps_madds1 on one lane: multiplier rounded to 25 bits, single-precision result.
inline double ps_mul(double a, double k) { return ppc::fs(ppc::fmul(a, k)); }
inline double ps_madd(double a, double k, double b) { return ppc::fs(ppc::fmadds(a, k, b)); }
}  // namespace


// void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab): ab = a * b (3x4 affine matrices).
static void concat_body(ppc::Context& c, uint8_t* m) {
  const uint32_t a = ARG0, b = ARG1, ab = ARG2;
  // Fast path while FPSCR.NI is clear (always, for this game): every paired-single step is one IEEE
  // operation and its rounding to single (ps_muls0 = (float)(B * a0) with the product exact in double,
  // ps_madds = (float)fma(B, a, t); f25 is the identity on single values), and psq_st flushes a
  // denormal result to zero. Only NaNs differ (operand-priority quieting); a NaN anywhere reaches the
  // outputs, so a NaN output (or NI set) takes the exact sequence below, nothing stored before.
  if (!(ppc::detail::current_fpscr & 4)) {
    double A[3][4], B[3][4], R[3][4];
    for (int r = 0; r < 3; ++r)
      for (int col = 0; col < 4; ++col) {
        A[r][col] = ppc::float_bits_to_double(ppc::ld32(c, m, a + r * 16 + col * 4));
        B[r][col] = ppc::float_bits_to_double(ppc::ld32(c, m, b + r * 16 + col * 4));
      }
    bool nan = false;
    for (int r = 0; r < 3; ++r) {
      const double a0 = A[r][0], a1 = A[r][1], a2 = A[r][2], a3 = A[r][3];
      for (int col = 0; col < 4; ++col) {
        double t = double(float(B[0][col] * a0));
        t = double(float(ppc::detail::hardware_madd(B[1][col], a1, t)));
        t = double(float(ppc::detail::hardware_madd(B[2][col], a2, t)));
        if (col >= 2) t = double(float(ppc::detail::hardware_madd(col == 3 ? 1.0 : 0.0, a3, t)));   // Unit01 lane
        R[r][col] = t;
        nan |= std::isnan(t);
      }
    }
    if (!nan) {
      for (int r = 0; r < 3; ++r) for (int col = 0; col < 4; ++col) ps_store(m, c, ab + r * 16 + col * 4, R[r][col]);
      return;
    }
  }
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
inline uint32_t pipe_pair_word(uint8_t* m, ppc::Context& c, uint32_t src) { return __builtin_bswap32(ppc::double_to_float_bits_ftz(ps_load(m, c, src))); }
inline uint32_t pipe_single_word(uint8_t* m, ppc::Context& c, uint32_t src) { return __builtin_bswap32(ppc::double_to_float_bits(ps_load(m, c, src))); }
}  // namespace

// static void WriteMTXPS4x3(f32 mtx[3][4], volatile f32* dest): six pairs.
// Each writer converts the whole matrix, then appends it to the capture in one call.
HLE(WriteMTXPS4x3) {
  const uint32_t mtx = ARG0;
  uint32_t w[12];
  for (uint32_t i = 0; i < 12; ++i) w[i] = pipe_pair_word(m, c, mtx + i * 4);
  gx::fifo_store_words(w, 12);
}
// static void WriteMTXPS3x3from3x4(f32 mtx[3][4], volatile f32* dest): per row a pair then a single.
HLE(WriteMTXPS3x3from3x4) {
  const uint32_t mtx = ARG0;
  uint32_t w[9];
  for (uint32_t row = 0; row < 3; ++row) {
    w[row * 3] = pipe_pair_word(m, c, mtx + row * 16); w[row * 3 + 1] = pipe_pair_word(m, c, mtx + row * 16 + 4);
    w[row * 3 + 2] = pipe_single_word(m, c, mtx + row * 16 + 8);
  }
  gx::fifo_store_words(w, 9);
}
// static void WriteMTXPS4x2(f32 mtx[2][4], volatile f32* dest): four pairs.
HLE(WriteMTXPS4x2) {
  const uint32_t mtx = ARG0;
  uint32_t w[8];
  for (uint32_t i = 0; i < 8; ++i) w[i] = pipe_pair_word(m, c, mtx + i * 4);
  gx::fifo_store_words(w, 8);
}

// void HSD_MtxInverseTranspose(Mtx src, Mtx dest): the inverse transpose of the 3x3 part, used for
// every lit joint's normal matrix (5.4% of the simulation thread in a two-character match). The
// operations below are the translated instruction sequence of 80379A20, one helper call per Gekko
// instruction in the same order with the same operands (the determinant expansion the compiler
// emitted, fabs through the integer sign mask, fdivs for 1/det, fmul/fmsubs/fnmsubs per element), so
// the results match the translation to the bit. Source elements are read before any store, which is
// what the original's copy to a stack temporary achieves when src == dest.
// Plain-IEEE form of the sequence below, valid while FPSCR.NI is clear (the game never sets it; see the
// fpscr log line): every helper then reduces to one IEEE operation rounded to double and, where the
// original rounds, to single: f25 of a single-precision value is the identity, fs is (float), finish is a
// no-op. Only NaNs differ (operand-priority quieting), and a NaN anywhere reaches the outputs, so any
// non-finite output sends the call to the exact sequence. Returns false (nothing stored) in that case.
static bool inverse_transpose_plain(ppc::Context& c, uint8_t* m, uint32_t src, uint32_t dest) {
  if (ppc::detail::current_fpscr & 4) return false;
  const auto L = [&](uint32_t off) { return ppc::float_bits_to_double(ppc::ld32(c, m, src + off)); };
  const auto r = [](double x) { return double(float(x)); };
  const double m00 = L(0), m01 = L(4), m02 = L(8), m10 = L(16), m11 = L(20), m12 = L(24), m20 = L(32), m21 = L(36), m22 = L(40);
  double f1 = r(m01 * m12);
  const double f4 = r(m10 * m01);
  double f2 = r(m00 * m11);
  f1 = r(m20 * f1);
  const double f3 = r(m02 * m10);
  f1 = r(ppc::detail::hardware_madd(m22, f2, f1));
  f2 = r(m20 * m11);
  const double f5 = r(m00 * m21);
  f1 = r(ppc::detail::hardware_madd(m21, f3, f1));
  f1 = -r(ppc::detail::hardware_madd(m02, f2, -f1));
  f1 = -r(ppc::detail::hardware_madd(m22, f4, -f1));
  const double det = -r(ppc::detail::hardware_madd(m12, f5, -f1));
  if (!std::isfinite(det)) return false;
  const double eps = ppc::float_bits_to_double(ppc::ld32(c, m, c.r[2] - 5000u));
  if (std::fabs(float(det)) < eps) return false;   // singular: the exact path copies (rare)
  const double inv = r(ppc::float_bits_to_double(ppc::ld32(c, m, c.r[2] - 4996u)) / det);
  double e00 = m00, e01 = m01, e02 = m02, e10 = m10, e11 = m11, e12 = m12, e20 = m20, e21 = m21, e22 = m22;
  if (src == dest) {
    const auto flush = [](double v) { return ppc::float_bits_to_double(ppc::double_to_float_bits_ftz(v)); };
    e00 = flush(m00); e01 = flush(m01); e02 = flush(m02); e10 = flush(m10); e11 = flush(m11);
    e12 = flush(m12); e20 = flush(m20); e21 = flush(m21); e22 = flush(m22);
  }
  const auto E = [&](double a, double b, double cc, double d, bool negate) {
    const double t = r(a * b);
    const double u = negate ? -r(ppc::detail::hardware_madd(cc, d, -t)) : r(ppc::detail::hardware_madd(cc, d, -t));
    return r(inv * u);
  };
  const double out[9] = {E(e21, e12, e11, e22, false), E(e21, e02, e01, e22, true), E(e11, e02, e01, e12, false),
                         E(e20, e12, e10, e22, true), E(e20, e02, e00, e22, false), E(e10, e02, e00, e12, true),
                         E(e20, e11, e10, e21, false), E(e20, e01, e00, e21, true), E(e10, e01, e00, e11, false)};
  for (double v : out) if (!std::isfinite(v)) return false;
  static const uint32_t offs[9] = {0, 16, 32, 4, 20, 36, 8, 24, 40};
  for (int i = 0; i < 9; ++i) ppc::st32(c, m, dest + offs[i], ppc::double_to_float_bits(out[i]));
  const uint32_t zero = ppc::double_to_float_bits(ppc::float_bits_to_double(ppc::ld32(c, m, c.r[2] - 4992u)));
  ppc::st32(c, m, dest + 12, zero); ppc::st32(c, m, dest + 28, zero); ppc::st32(c, m, dest + 44, zero);
  return true;
}

static void inverse_transpose_body(ppc::Context& c, uint8_t* m) {
  const uint32_t src = ARG0, dest = ARG1;
  if (inverse_transpose_plain(c, m, src, dest)) return;
  const auto L = [&](uint32_t off) { return ppc::float_bits_to_double(ppc::ld32(c, m, src + off)); };
  const auto S = [&](uint32_t off, double v) { ppc::st32(c, m, dest + off, ppc::double_to_float_bits(v)); };
  using ppc::fs; using ppc::fmul; using ppc::f25; using ppc::fmadds; using ppc::fmsubs; using ppc::fnmsubs;
  const double m00 = L(0), m01 = L(4), m02 = L(8), m10 = L(16), m11 = L(20), m12 = L(24), m20 = L(32), m21 = L(36), m22 = L(40);
  // Determinant (80379A38..80379A8C).
  double f1 = fs(fmul(m01, f25(m12)));
  const double f4 = fs(fmul(m10, f25(m01)));
  double f2 = fs(fmul(m00, f25(m11)));
  f1 = fs(fmul(m20, f25(f1)));
  const double f3 = fs(fmul(m02, f25(m10)));
  f1 = fmadds(m22, f25(f2), f1);
  f2 = fs(fmul(m20, f25(m11)));
  const double eps = ppc::float_bits_to_double(ppc::ld32(c, m, c.r[2] - 5000u));
  const double f5 = fs(fmul(m00, f25(m21)));
  f1 = fmadds(m21, f25(f3), f1);
  f1 = fnmsubs(m02, f25(f2), f1);
  f1 = fnmsubs(m22, f25(f4), f1);
  const double det = fnmsubs(m12, f25(f5), f1);
  const double mag = ppc::float_bits_to_double(ppc::double_to_float_bits(det) & 0x7FFFFFFFu);
  if (mag < eps) {   // fcmpu ... blt: singular, dest = src (PSMTXCopy: psq_l/psq_st pairs)
    if (src != dest)
      for (uint32_t off = 0; off < 48; off += 4) ps_store(m, c, dest + off, ps_load(m, c, src + off));
    return;
  }
  const double inv = fs(ppc::fdiv(ppc::float_bits_to_double(ppc::ld32(c, m, c.r[2] - 4996u)), det));
  // In place, the original first copies src to a stack temporary with PSMTXCopy (psq_st flushes
  // denormals to zero) and reads the elements from there; the determinant above used src itself.
  double e00 = m00, e01 = m01, e02 = m02, e10 = m10, e11 = m11, e12 = m12, e20 = m20, e21 = m21, e22 = m22;
  if (src == dest) {
    const auto flush = [](double v) { return ppc::float_bits_to_double(ppc::double_to_float_bits_ftz(v)); };
    e00 = flush(m00); e01 = flush(m01); e02 = flush(m02); e10 = flush(m10); e11 = flush(m11);
    e12 = flush(m12); e20 = flush(m20); e21 = flush(m21); e22 = flush(m22);
  }
  // Each element: f1 = a*b; f1 = c*d -/+ f1 (fmsubs or fnmsubs); f1 = inv*f1.
  const auto E = [&](uint32_t off, double a, double b, double cc, double d, bool negate) {
    const double t = fs(fmul(a, f25(b)));
    const double u = negate ? fnmsubs(cc, f25(d), t) : fmsubs(cc, f25(d), t);
    S(off, fs(fmul(inv, f25(u))));
  };
  E(0, e21, e12, e11, e22, false);
  E(16, e21, e02, e01, e22, true);
  E(32, e11, e02, e01, e12, false);
  E(4, e20, e12, e10, e22, true);
  E(20, e20, e02, e00, e22, false);
  E(36, e10, e02, e00, e12, true);
  E(8, e20, e11, e10, e21, false);
  E(24, e20, e01, e00, e21, true);
  E(40, e10, e01, e00, e11, false);
  const double zero = ppc::float_bits_to_double(ppc::ld32(c, m, c.r[2] - 4992u));
  S(12, zero); S(28, zero); S(44, zero);
}

namespace {
// hle_verify=1 (MELEE_HLE_VERIFY=1): every call of a hooked matrix routine also runs the original
// game code through the interpreter (which uses the same numeric helpers as the translation) on the
// same inputs, and logs the first differing calls. The interpreter's result is what stays in memory.
// Read at the first call, not at static initialisation: main sets the variable from launcher.ini.
bool verify_enabled() { static const bool on = [] { const char* v = std::getenv("MELEE_HLE_VERIFY"); return v && v[0] == '1'; }(); return on; }
template <class Body>
void verified(ppc::Context& c, uint8_t* m, const char* name, uint32_t guest, uint32_t src_a, uint32_t src_b, uint32_t dest, Body body) {
  if (!verify_enabled()) { body(c, m); return; }
  static uint64_t calls = 0, mismatches = 0;
  uint8_t* d = host::ram + (dest & 0x01FFFFFF);
  uint8_t* a = host::ram + (src_a & 0x01FFFFFF);
  uint8_t* b = src_b ? host::ram + (src_b & 0x01FFFFFF) : nullptr;
  uint8_t keep_d[48], keep_a[48], keep_b[48], mine[48];
  std::memcpy(keep_d, d, 48); std::memcpy(keep_a, a, 48); if (b) std::memcpy(keep_b, b, 48);
  const ppc::Context saved = c;
  body(c, m);
  std::memcpy(mine, d, 48);
  std::memcpy(d, keep_d, 48); std::memcpy(a, keep_a, 48); if (b) std::memcpy(b, keep_b, 48);
  ppc::Context ref = saved;
  ppc::interpret(ref, m, guest);
  ++calls;
  if (std::memcmp(mine, d, 48)) {
    if (++mismatches <= 8) {
      char line[400]; int n = std::snprintf(line, sizeof line, "hle-verify %s MISMATCH call %llu:", name, (unsigned long long)calls);
      for (int i = 0; i < 12 && n < (int)sizeof line - 24; ++i) {
        uint32_t x, y; std::memcpy(&x, mine + i * 4, 4); std::memcpy(&y, d + i * 4, 4);
        if (x != y) n += std::snprintf(line + n, sizeof line - n, " [%d] %08X/%08X", i, __builtin_bswap32(x), __builtin_bswap32(y));
      }
      host::log("%s", line);
    }
  }
  if ((calls & (calls - 1)) == 0 && calls >= 1024) host::log("hle-verify %s: %llu calls, %llu mismatches", name, (unsigned long long)calls, (unsigned long long)mismatches);
}
}  // namespace

HLE(PSMTXConcat) { verified(c, m, "PSMTXConcat", 0x80342204u, ARG0, ARG1, ARG2, concat_body); }
HLE(HSD_MtxInverseTranspose) { verified(c, m, "HSD_MtxInverseTranspose", 0x80379A20u, ARG0, 0, ARG1, inverse_transpose_body); }
