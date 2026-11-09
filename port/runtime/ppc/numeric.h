// Portable numeric primitives for the AOT Gekko runtime.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

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

// Strict FP flags are required for every TU that includes this header: the
// inline helpers below depend on the dynamic rounding mode and on no contraction.
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace ppc {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
              sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "The Gekko runtime requires IEEE binary32/binary64");

constexpr uint16_t bswap16(uint16_t v) { return uint16_t((v << 8) | (v >> 8)); }
constexpr uint32_t bswap32(uint32_t v) {
  return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
         ((v >> 8) & 0xff00u) | (v >> 24);
}
constexpr uint64_t bswap64(uint64_t v) {
  return (uint64_t(bswap32(uint32_t(v))) << 32) | bswap32(uint32_t(v >> 32));
}
constexpr uint32_t rotl32(uint32_t v, uint32_t n) {
  n &= 31;
  return (v << n) | (v >> ((0u - n) & 31));
}
constexpr uint32_t cntlzw(uint32_t v) {
  uint32_t n = 0;
  if (!v) return 32;
  while (!(v & 0x80000000u)) { ++n; v <<= 1; }
  return n;
}
constexpr int64_t signed_word(uint32_t v) {
  return int64_t(v) - ((v & 0x80000000u) ? INT64_C(0x100000000) : 0);
}
constexpr uint32_t mulhw(uint32_t a, uint32_t b) {
  // Multiplication fits in int64; extraction uses an unsigned shift.
  return uint32_t(uint64_t(signed_word(a) * signed_word(b)) >> 32);
}
constexpr uint32_t arithmetic_shift_right(uint32_t v, uint32_t n) {
  n &= 31;
  if (!n) return v;
  return (v >> n) | ((v & 0x80000000u) ? (~uint32_t(0) << (32 - n)) : 0u);
}
inline double bits_to_double(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
inline uint64_t double_to_bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

// ReferenceSlippi is canonical on every host. It implements the return-value rules
// cited in numeric.cpp; FPSCR exception/FPRF side effects remain unimplemented.
// UnlockedJit64 preserves the original x86 FMA/FTZ+DAZ behavior for diagnostics.
// It is not a supported cross-host synchronization profile.
enum class FpProfile { ReferenceSlippi, UnlockedJit64 };
FpProfile fp_profile();
const char* fp_profile_name(FpProfile profile);
uint32_t guest_fpscr();
void set_fp_environment(uint32_t fpscr, FpProfile profile = FpProfile::ReferenceSlippi);

// Construct at the outer guest entry on EACH executing thread. FPSCR writes call
// set_fp_environment again. Host state and the prior thread-local guest state are
// restored on scope exit, including exception unwinding.
class ScopedGuestFpEnvironment {
public:
  explicit ScopedGuestFpEnvironment(uint32_t fpscr,
                                   FpProfile profile = FpProfile::ReferenceSlippi);
  ~ScopedGuestFpEnvironment();
  ScopedGuestFpEnvironment(const ScopedGuestFpEnvironment&) = delete;
  ScopedGuestFpEnvironment& operator=(const ScopedGuestFpEnvironment&) = delete;
private:
  std::fenv_t host_env_{};
  uint64_t host_control_ = 0;
  uint32_t previous_fpscr_ = 0;
  FpProfile previous_profile_ = FpProfile::ReferenceSlippi;
};

// Hot helpers are inline so every recompiled FP instruction inlines them. Their
// bodies (and the rules they implement) are documented in numeric.cpp's header.
namespace detail {
constexpr uint64_t sign_bit = UINT64_C(0x8000000000000000);
constexpr uint64_t exp_mask = UINT64_C(0x7ff0000000000000);
constexpr uint64_t fraction_mask = UINT64_C(0x000fffffffffffff);
constexpr uint64_t quiet_bit = UINT64_C(0x0008000000000000);
// Owned by set_fp_environment / ScopedGuestFpEnvironment in numeric.cpp.
inline thread_local uint32_t current_fpscr = 0;
inline thread_local FpProfile current_profile = FpProfile::ReferenceSlippi;

// An unordered self-compare: one fcmp, no move of the value into an integer register (that move ran
// under every translated float operation).
[[gnu::always_inline]] inline bool is_nan(double d) { return __builtin_isnan(d); }
[[gnu::always_inline]] inline double quiet(double d) {
  return bits_to_double(double_to_bits(d) | quiet_bit);
}
[[gnu::always_inline]] inline bool reference() {
#if defined(__SWITCH__)
  // The Switch build only ever runs the reference profile (set_fp_environment's default, never
  // overridden there); a constant drops the legacy branches and a TLS load from every float helper.
  return true;
#else
  return current_profile == FpProfile::ReferenceSlippi;
#endif
}
[[gnu::always_inline]] inline double finish(double d) {
  if (reference() && (current_fpscr & 4) && !(double_to_bits(d) & exp_mask))
    return bits_to_double(double_to_bits(d) & sign_bit);
  return d;
}
// The NaN and denormal paths below are rare and out of line ([[gnu::cold]], noinline): inlined, they
// were copied into every translated float operation (a 49-instruction guest function compiled to
// 3,000 ARM instructions), which kept the game code far larger than the instruction cache.
[[gnu::cold, gnu::noinline]] inline double nan_binary(double a, double b) {
  if (is_nan(a)) return quiet(a);
  if (is_nan(b)) return quiet(b);
  return bits_to_double(exp_mask | quiet_bit);
}
[[gnu::cold, gnu::noinline]] inline double nan_fused(double a, double c, double b) {
  // PowerPC operand priority is A, B, C even though the operation is A*C+B.
  if (is_nan(a)) return quiet(a);
  if (is_nan(b)) return quiet(b);
  if (is_nan(c)) return quiet(c);
  return bits_to_double(exp_mask | quiet_bit);
}
[[gnu::always_inline]] inline double binary_result(double result, double a, double b) {
  if (reference() && __builtin_expect(is_nan(result), 0)) return nan_binary(a, b);
  return finish(result);
}
[[gnu::always_inline]] inline double fused_result(double result, double a, double c, double b) {
  if (reference() && __builtin_expect(is_nan(result), 0)) return nan_fused(a, c, b);
  return finish(result);
}
[[gnu::always_inline]] inline double negate_result(double d) {
  return is_nan(d) ? d : bits_to_double(double_to_bits(d) ^ sign_bit);
}
[[gnu::always_inline]] inline double hardware_madd(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#elif defined(PPC_HOST_ARM64) && !defined(_MSC_VER)
  double r;
  // Not volatile: the instruction has no side effects beyond its result, so the compiler may schedule
  // and combine it; it is still always this one fused instruction.
  __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(c), "w"(b));
  return r;
#else
  // Correctly fused fallback; never replace this with a*c+b.
  return std::fma(a, c, b);
#endif
}
[[gnu::always_inline]] inline double hardware_msub(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fmsub_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
  return hardware_madd(a, c, bits_to_double(double_to_bits(b) ^ sign_bit));
#endif
}
inline double legacy_nmadd(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fnmsub_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
  return hardware_madd(-a, c, -b);
#endif
}
inline double legacy_nmsub(double a, double c, double b) {
#if defined(PPC_HOST_X86) && (defined(__FMA__) || defined(_MSC_VER))
  return _mm_cvtsd_f64(_mm_fnmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#else
  return hardware_madd(-a, c, b);
#endif
}
}  // namespace detail

[[gnu::always_inline]] inline double fs(double d) {
  using namespace detail;
  if (reference() && (current_fpscr & 4) &&
      (double_to_bits(d) & ~sign_bit) < UINT64_C(0x3810000000000000))
    return bits_to_double(double_to_bits(d) & sign_bit);
  return double(float(d));
}
namespace detail {
[[gnu::cold, gnu::noinline]] inline double f25_denormal(uint64_t bits) {
  uint64_t keep = UINT64_C(0xfffffffff8000000), round = UINT64_C(0x8000000);
  uint64_t fraction = bits & fraction_mask;
  unsigned shift = 0;
  while (!(fraction & (UINT64_C(1) << 52))) { fraction <<= 1; ++shift; }
  // Arithmetic right shift of the signed keep mask, expressed unsigned.
  keep = ~(~keep >> shift);
  round >>= shift;
  return bits_to_double((bits & keep) + (bits & round));
}
}  // namespace detail
[[gnu::always_inline]] inline double f25(double d) {
  using namespace detail;
  const uint64_t bits = double_to_bits(d);
  if (reference() && __builtin_expect(!(bits & exp_mask) && (bits & fraction_mask), 0)) return f25_denormal(bits);
  const uint64_t keep = UINT64_C(0xfffffffff8000000), round = UINT64_C(0x8000000);
  return bits_to_double((bits & keep) + (bits & round));
}
[[gnu::always_inline]] inline double fadd(double a, double b) { return detail::binary_result(a + b, a, b); }
[[gnu::always_inline]] inline double fsub(double a, double b) { return detail::binary_result(a - b, a, b); }
[[gnu::always_inline]] inline double fmul(double a, double b) { return detail::binary_result(a * b, a, b); }
[[gnu::always_inline]] inline double fdiv(double a, double b) { return detail::binary_result(a / b, a, b); }
[[gnu::always_inline]] inline double fmadd(double a, double c, double b) {
  return detail::fused_result(detail::hardware_madd(a, c, b), a, c, b);
}
[[gnu::always_inline]] inline double fmsub(double a, double c, double b) {
  return detail::fused_result(detail::hardware_msub(a, c, b), a, c, b);
}
[[gnu::always_inline]] inline double fnmadd(double a, double c, double b) {
  return detail::reference() ? detail::negate_result(fmadd(a, c, b)) : detail::legacy_nmadd(a, c, b);
}
[[gnu::always_inline]] inline double fnmsub(double a, double c, double b) {
  return detail::reference() ? detail::negate_result(fmsub(a, c, b)) : detail::legacy_nmsub(a, c, b);
}
// Single-result FMA helpers take an already Force25Bit-rounded multiplier.
// Separate entry points keep rounding BEFORE negation, including directed RN.
[[gnu::always_inline]] inline double fmadds(double a, double c, double b) { return fs(fmadd(a, c, b)); }
[[gnu::always_inline]] inline double fmsubs(double a, double c, double b) { return fs(fmsub(a, c, b)); }
[[gnu::always_inline]] inline double fnmadds(double a, double c, double b) {
  return detail::reference() ? detail::negate_result(fmadds(a, c, b)) : fs(detail::legacy_nmadd(a, c, b));
}
[[gnu::always_inline]] inline double fnmsubs(double a, double c, double b) {
  return detail::reference() ? detail::negate_result(fmsubs(a, c, b)) : fs(detail::legacy_nmsub(a, c, b));
}
inline uint64_t fctiw(double b, bool truncate) {
  using namespace detail;
  const double rounded = truncate ? std::trunc(b) : std::nearbyint(b);
  uint32_t v;
  if (is_nan(b) || rounded < -2147483648.0) v = 0x80000000u;
  else if (rounded >= 2147483648.0) v = 0x7fffffffu;
  else v = uint32_t(int32_t(rounded));
  uint64_t result = UINT64_C(0xfff8000000000000) | v;
  if (reference() && v == 0 && (double_to_bits(b) & sign_bit)) result |= UINT64_C(0x100000000);
  return result;
}
namespace detail {
[[gnu::cold, gnu::noinline]] inline double float_bits_to_double_special(uint32_t value) {
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
}  // namespace detail
// lfs: every single except a NaN converts exactly in hardware (FPCR.FZ is clear, so denormals too),
// which is what the bit construction in the special path computes; only NaNs differ, since the host
// conversion quiets a signalling NaN and the Gekko keeps its payload.
[[gnu::always_inline]] inline double float_bits_to_double(uint32_t value) {
  if (!detail::reference() || __builtin_expect((value & 0x7f800000u) != 0x7f800000u || !(value & 0x7fffffu), 1)) {
    float f; std::memcpy(&f, &value, 4); return double(f);
  }
  return detail::float_bits_to_double_special(value);
}
[[gnu::always_inline]] inline uint32_t double_to_float_bits(double d) {
  using namespace detail;
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
[[gnu::always_inline]] inline uint32_t double_to_float_bits_ftz(double d) {
  using namespace detail;
  if (!reference()) return double_to_float_bits(d);
  const uint64_t x = double_to_bits(d);
  if (((x >> 52) & 0x7ff) <= 896 && (x & ~sign_bit)) return uint32_t((x >> 32) & 0x80000000);
  return double_to_float_bits(d);
}
// Table estimates stay out of line in numeric.cpp.
double fres(double value);
double frsqrte(double value);
}  // namespace ppc
