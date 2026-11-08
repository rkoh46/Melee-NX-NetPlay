// Copyright 2009, 2018, 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Portable return-value rules adapted from pinned Reference Slippi:
// https://github.com/project-slippi/dolphin/tree/41a7a3a110ed52999486ae1901c8fbb9a63d4f13
// Source/Core/Core/PowerPC/Interpreter/Interpreter_FPUtils.h (ForceSingle,
// Force25Bit, NI arithmetic, ConvertToSingle/Double), Interpreter_FloatingPoint.cpp
// (round-before-negation and integer conversion), Common/{Arm,x64}FPURoundMode.cpp.
// Changes: no emulator state/JIT dependencies; explicit thread-local environment;
// unsigned bit operations; return values only (NOT FPSCR exception/status flags).
// The original Unlocked x86 behavior is a separate diagnostic profile.
#include "numeric.h"
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define PPC_HOST_X86 1
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define PPC_HOST_ARM64 1
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#error "PPC floating-point environment is only implemented for x86 and arm64"
#endif

// Strict FP flags are required for callers as well as this translation unit.
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace ppc {
namespace {
constexpr uint64_t sign_bit = UINT64_C(0x8000000000000000);
constexpr uint64_t exp_mask = UINT64_C(0x7ff0000000000000);
constexpr uint64_t fraction_mask = UINT64_C(0x000fffffffffffff);
constexpr uint64_t quiet_bit = UINT64_C(0x0008000000000000);
thread_local uint32_t current_fpscr = 0;
thread_local FpProfile current_profile = FpProfile::ReferenceSlippi;

uint64_t read_control() {
#if defined(PPC_HOST_X86)
  return _mm_getcsr();
#elif defined(_MSC_VER)
  return _ReadStatusReg(ARM64_FPCR);
#else
  uint64_t v;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
  return v;
#endif
}
void write_control(uint64_t v) {
#if defined(PPC_HOST_X86)
  _mm_setcsr(uint32_t(v));
#elif defined(_MSC_VER)
  _WriteStatusReg(ARM64_FPCR, v);
#else
  __asm__ __volatile__("msr fpcr, %0" : : "r"(v) : "memory");
#endif
}
bool is_nan(double d) {
  return (double_to_bits(d) & ~sign_bit) > exp_mask;
}
double quiet(double d) { return bits_to_double(double_to_bits(d) | quiet_bit); }
bool reference() { return current_profile == FpProfile::ReferenceSlippi; }
double finish(double d) {
  if (reference() && (current_fpscr & 4) && !(double_to_bits(d) & exp_mask))
    return bits_to_double(double_to_bits(d) & sign_bit);
  return d;
}
double binary_result(double result, double a, double b) {
  if (reference() && is_nan(result)) {
    if (is_nan(a)) return quiet(a);
    if (is_nan(b)) return quiet(b);
    return bits_to_double(exp_mask | quiet_bit);
  }
  return finish(result);
}
double fused_result(double result, double a, double c, double b) {
  if (reference() && is_nan(result)) {
    // PowerPC operand priority is A, B, C even though the operation is A*C+B.
    if (is_nan(a)) return quiet(a);
    if (is_nan(b)) return quiet(b);
    if (is_nan(c)) return quiet(c);
    return bits_to_double(exp_mask | quiet_bit);
  }
  return finish(result);
}
double negate_result(double d) {
  return is_nan(d) ? d : bits_to_double(double_to_bits(d) ^ sign_bit);
}
double hardware_madd(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#elif defined(PPC_HOST_ARM64) && !defined(_MSC_VER)
  double r;
  __asm__ __volatile__("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(c), "w"(b));
  return r;
#else
  // Correctly fused fallback; never replace this with a*c+b.
  return std::fma(a, c, b);
#endif
}
double hardware_msub(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fmsub_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
  return hardware_madd(a, c, bits_to_double(double_to_bits(b) ^ sign_bit));
#endif
}
double legacy_nmadd(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fnmsub_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
  return hardware_madd(-a, c, -b);
#endif
}
double legacy_nmsub(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fnmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
  return hardware_madd(-a, c, b);
#endif
}
}  // namespace

FpProfile fp_profile() { return current_profile; }
uint32_t guest_fpscr() { return current_fpscr; }
const char* fp_profile_name(FpProfile p) {
  return p == FpProfile::ReferenceSlippi ? "reference-slippi-values (FPSCR status incomplete)" :
                                         "unlocked-jit64-diagnostic (cross-host unverified)";
}
void set_fp_environment(uint32_t fpscr, FpProfile profile) {
  current_fpscr = fpscr;
  current_profile = profile;
  static const int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
  std::fesetround(modes[fpscr & 3]);
  uint64_t control = read_control();
  const bool legacy_flush = profile == FpProfile::UnlockedJit64 && (fpscr & 4);
#if defined(PPC_HOST_X86)
  // Reference inputs must remain unflushed. Reference NI output flushing is
  // handled explicitly, also covering single-conversion pre-rounding quirks.
  control &= ~(UINT64_C(0x8000) | UINT64_C(0x40));
  control |= UINT64_C(0x1f80);  // mask host exceptions; guest flags are separate
  if (legacy_flush) control |= UINT64_C(0x8040);
#else
  // No dependency on FEAT_AFP: leave both input and output denormals enabled.
  // Clear default-NaN, FZ/FZ16, AH/FIZ, and exception enables. Software helpers
  // implement reference NI outputs, preserving subnormal inputs on older Macs.
  control &= ~((UINT64_C(1) << 25) | (UINT64_C(1) << 24) | (UINT64_C(1) << 19) |
               UINT64_C(3) | UINT64_C(0x9f00));
  if (legacy_flush) control |= UINT64_C(1) << 24;
#endif
  write_control(control);
}
ScopedGuestFpEnvironment::ScopedGuestFpEnvironment(uint32_t fpscr, FpProfile profile)
    : host_control_(read_control()), previous_fpscr_(current_fpscr),
      previous_profile_(current_profile) {
  std::feholdexcept(&host_env_);
  set_fp_environment(fpscr, profile);
}
ScopedGuestFpEnvironment::~ScopedGuestFpEnvironment() {
  std::fesetenv(&host_env_);
  write_control(host_control_);
  current_fpscr = previous_fpscr_;
  current_profile = previous_profile_;
}

double fs(double d) {
  if (reference() && (current_fpscr & 4) &&
      (double_to_bits(d) & ~sign_bit) < UINT64_C(0x3810000000000000))
    return bits_to_double(double_to_bits(d) & sign_bit);
  return double(float(d));
}
double f25(double d) {
  uint64_t bits = double_to_bits(d);
  uint64_t keep = UINT64_C(0xfffffffff8000000), round = UINT64_C(0x8000000);
  uint64_t fraction = bits & fraction_mask;
  if (reference() && !(bits & exp_mask) && fraction) {
    unsigned shift = 0;
    while (!(fraction & (UINT64_C(1) << 52))) { fraction <<= 1; ++shift; }
    // Arithmetic right shift of the signed keep mask, expressed unsigned.
    keep = ~(~keep >> shift);
    round >>= shift;
  }
  return bits_to_double((bits & keep) + (bits & round));
}
double fadd(double a, double b) { return binary_result(a + b, a, b); }
double fsub(double a, double b) { return binary_result(a - b, a, b); }
double fmul(double a, double b) { return binary_result(a * b, a, b); }
double fdiv(double a, double b) { return binary_result(a / b, a, b); }
double fmadd(double a, double c, double b) { return fused_result(hardware_madd(a, c, b), a, c, b); }
double fmsub(double a, double c, double b) { return fused_result(hardware_msub(a, c, b), a, c, b); }
double fnmadd(double a, double c, double b) {
  return reference() ? negate_result(fmadd(a, c, b)) : legacy_nmadd(a, c, b);
}
double fnmsub(double a, double c, double b) {
  return reference() ? negate_result(fmsub(a, c, b)) : legacy_nmsub(a, c, b);
}
double fmadds(double a, double c, double b) { return fs(fmadd(a, c, b)); }
double fmsubs(double a, double c, double b) { return fs(fmsub(a, c, b)); }
double fnmadds(double a, double c, double b) {
  return reference() ? negate_result(fmadds(a, c, b)) : fs(legacy_nmadd(a, c, b));
}
double fnmsubs(double a, double c, double b) {
  return reference() ? negate_result(fmsubs(a, c, b)) : fs(legacy_nmsub(a, c, b));
}
uint64_t fctiw(double b, bool truncate) {
  const double rounded = truncate ? std::trunc(b) : std::nearbyint(b);
  uint32_t v;
  if (is_nan(b) || rounded < -2147483648.0) v = 0x80000000u;
  else if (rounded >= 2147483648.0) v = 0x7fffffffu;
  else v = uint32_t(int32_t(rounded));
  uint64_t result = UINT64_C(0xfff8000000000000) | v;
  if (reference() && v == 0 && (double_to_bits(b) & sign_bit)) result |= UINT64_C(0x100000000);
  return result;
}
double float_bits_to_double(uint32_t value) {
  if (!reference()) { float f; std::memcpy(&f, &value, 4); return double(f); }
  uint64_t x = value, exp = (x >> 23) & 255, frac = x & 0x7fffff;
  if (!exp && frac) {
    exp = 1023 - 126;
    do { frac <<= 1; --exp; } while (!(frac & 0x800000));
    return bits_to_double(((x & 0x80000000) << 32) | (exp << 52) | ((frac & 0x7fffff) << 29));
  }
  const uint64_t y = (exp && exp < 255) ? !(exp >> 7) : exp >> 7;
  return bits_to_double(((x & 0xc0000000) << 32) | (y << 61) | (y << 60) | (y << 59) |
                        ((x & 0x3fffffff) << 29));
}
uint32_t double_to_float_bits(double d) {
  if (!reference()) { float f = float(d); uint32_t u; std::memcpy(&u, &f, 4); return u; }
  const uint64_t x = double_to_bits(d);
  const uint32_t exp = uint32_t((x >> 52) & 0x7ff);
  if (exp <= 896 && exp >= 874 && (x & ~sign_bit)) {
    const uint32_t t = uint32_t(0x80000000u | ((x & fraction_mask) >> 21));
    return (t >> (905 - exp)) | uint32_t((x >> 32) & 0x80000000u);
  }
  // The very-small/out-of-range encodings follow the pinned HW-test rule;
  // this is a store conversion, deliberately not arithmetic float rounding.
  return uint32_t(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
}
uint32_t double_to_float_bits_ftz(double d) {
  if (!reference()) return double_to_float_bits(d);
  const uint64_t x = double_to_bits(d);
  if (((x >> 52) & 0x7ff) <= 896 && (x & ~sign_bit)) return uint32_t((x >> 32) & 0x80000000);
  return double_to_float_bits(d);
}
// Table estimates, adapted from pinned Common/FloatUtils.cpp. NaNs are quieted
// explicitly; no reciprocal/sqrt host estimate is substituted.
static const int frsqrte_expected_base[] = {
  0x3ffa000, 0x3c29000, 0x38aa000, 0x3572000, 0x3279000, 0x2fb7000, 0x2d26000, 0x2ac0000,
  0x2881000, 0x2665000, 0x2468000, 0x2287000, 0x20c1000, 0x1f12000, 0x1d79000, 0x1bf4000,
  0x1a7e800, 0x17cb800, 0x1552800, 0x130c000, 0x10f2000, 0x0eff000, 0x0d2e000, 0x0b7c000,
  0x09e5000, 0x0867000, 0x06ff000, 0x05ab800, 0x046a000, 0x0339800, 0x0218800, 0x0105800,
};
static const int frsqrte_expected_dec[] = {
  0x7a4, 0x700, 0x670, 0x5f2, 0x584, 0x524, 0x4cc, 0x47e, 0x43a, 0x3fa, 0x3c2, 0x38e,
  0x35e, 0x332, 0x30a, 0x2e6, 0x568, 0x4f3, 0x48d, 0x435, 0x3e7, 0x3a2, 0x365, 0x32e,
  0x2fc, 0x2d0, 0x2a8, 0x283, 0x261, 0x243, 0x226, 0x20b,
};

double frsqrte(double val) {
  uint64_t vali = double_to_bits(val);
  uint64_t mantissa = vali & ((UINT64_C(1) << 52) - 1);
  uint64_t sign = vali & (UINT64_C(1) << 63);
  int64_t exponent = int64_t(vali & (UINT64_C(0x7ff) << 52));
  if (mantissa == 0 && exponent == 0)
    return sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0) return sign ? bits_to_double(UINT64_C(0x7ff8000000000000)) : 0.0;
    return bits_to_double(vali | UINT64_C(0x0008000000000000));
  }
  if (sign) return bits_to_double(UINT64_C(0x7ff8000000000000));
  if (!exponent) {
    do { exponent -= 1LL << 52; mantissa <<= 1; } while (!(mantissa & (1LL << 52)));
    mantissa &= (1LL << 52) - 1;
    exponent += 1LL << 52;
  }
  bool odd_exponent = !(exponent & (1LL << 52));
  exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
  int i = (int)(mantissa >> 37);
  vali = sign | uint64_t(exponent);
  int index = i / 2048 + (odd_exponent ? 16 : 0);
  vali |= (uint64_t)(frsqrte_expected_base[index] - frsqrte_expected_dec[index] * (i % 2048)) << 26;
  double out; std::memcpy(&out, &vali, 8);
  return out;
}

static const int fres_expected_base[] = {
  0x7ff800, 0x783800, 0x70ea00, 0x6a0800, 0x638800, 0x5d6200, 0x579000, 0x520800,
  0x4cc800, 0x47ca00, 0x430800, 0x3e8000, 0x3a2c00, 0x360800, 0x321400, 0x2e4a00,
  0x2aa800, 0x272c00, 0x23d600, 0x209e00, 0x1d8800, 0x1a9000, 0x17ae00, 0x14f800,
  0x124400, 0x0fbe00, 0x0d3800, 0x0ade00, 0x088400, 0x065000, 0x041c00, 0x020c00,
};
static const int fres_expected_dec[] = {
  0x3e1, 0x3a7, 0x371, 0x340, 0x313, 0x2ea, 0x2c4, 0x2a0, 0x27f, 0x261, 0x245, 0x22a,
  0x212, 0x1fb, 0x1e5, 0x1d1, 0x1be, 0x1ac, 0x19b, 0x18b, 0x17c, 0x16e, 0x15b, 0x15b,
  0x143, 0x143, 0x12d, 0x12d, 0x11a, 0x11a, 0x108, 0x106,
};

double fres(double val) {
  uint64_t vali = double_to_bits(val);
  uint64_t mantissa = vali & ((UINT64_C(1) << 52) - 1);
  uint64_t sign = vali & (UINT64_C(1) << 63);
  int64_t exponent = int64_t(vali & (UINT64_C(0x7ff) << 52));
  if (mantissa == 0 && exponent == 0) return std::copysign(std::numeric_limits<double>::infinity(), val);
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0) return std::copysign(0.0, val);
    return bits_to_double(vali | UINT64_C(0x0008000000000000));
  }
  if (exponent < (895LL << 52)) return std::copysign((double)std::numeric_limits<float>::max(), val);
  if (exponent >= (1149LL << 52)) return std::copysign(0.0, val);
  exponent = (0x7FDLL << 52) - exponent;
  int i = (int)(mantissa >> 37);
  vali = sign | uint64_t(exponent);
  vali |= (uint64_t)(fres_expected_base[i / 1024] - (fres_expected_dec[i / 1024] * (i % 1024) + 1) / 2) << 29;
  double out; std::memcpy(&out, &vali, 8);
  return out;
}

}  // namespace ppc
