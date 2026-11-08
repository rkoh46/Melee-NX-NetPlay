// Portable numeric primitives for the AOT Gekko runtime.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cfenv>
#include <cstdint>
#include <cstring>
#include <limits>

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

double fs(double value);
double f25(double value);
double fadd(double a, double b);
double fsub(double a, double b);
double fmul(double a, double b);
double fdiv(double a, double b);
double fmadd(double a, double c, double b);
double fmsub(double a, double c, double b);
double fnmadd(double a, double c, double b);
double fnmsub(double a, double c, double b);
// Single-result FMA helpers take an already Force25Bit-rounded multiplier.
// Separate entry points keep rounding BEFORE negation, including directed RN.
double fmadds(double a, double c, double b);
double fmsubs(double a, double c, double b);
double fnmadds(double a, double c, double b);
double fnmsubs(double a, double c, double b);
uint64_t fctiw(double value, bool truncate);
double float_bits_to_double(uint32_t value);
uint32_t double_to_float_bits(double value);
uint32_t double_to_float_bits_ftz(double value);
double fres(double value);
double frsqrte(double value);
}  // namespace ppc
