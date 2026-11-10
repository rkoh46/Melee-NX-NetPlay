/* Inline guest-memory primitives for code lowered by GuestMemLower (docs/NATIVE_C.md). Compiled to
   IR for the console and linked into the lowered module, so every access inlines to a range check,
   a load or store and a byte swap. The pointers are set by the host glue (gml_glue.cpp). */
#include <stdint.h>
#define GML_INLINE __attribute__((always_inline))
extern uint8_t* gml_ram;      /* guest RAM (24 MiB at 0x80000000, mirrored at 0x00000000/0xC0000000) */
extern uint32_t* gml_regs;    /* the guest GPRs (ppc::Context::r) */
extern double* gml_fregs;     /* the guest FPRs (ppc::Context::f, two doubles each) */
extern uint8_t* gml_cr;       /* the guest CR fields */
uint32_t gml_ld_slow(uint32_t addr, uint32_t bytes);
uint64_t gml_ld64_slow(uint32_t addr);
void gml_st_slow(uint32_t addr, uint32_t value, uint32_t bytes);
void gml_st64_slow(uint32_t addr, uint64_t value);
void gml_guest_call(uint32_t addr);
void gml_panic(void);
void gml_memcpy_slow(uint32_t dst, uint32_t src, uint32_t n);
void gml_memset_slow(uint32_t dst, uint32_t value, uint32_t n);
#define RAM 0x01800000u
static inline __attribute__((always_inline)) int in_ram(uint32_t a, uint32_t n) { return (a & 0x3FFFFFFFu) + n <= RAM; }
GML_INLINE uint8_t gml_ld8(uint32_t a) { if (__builtin_expect(in_ram(a, 1), 1)) return gml_ram[a & 0x3FFFFFFFu]; return (uint8_t)gml_ld_slow(a, 1); }
GML_INLINE uint16_t gml_ld16(uint32_t a) { if (__builtin_expect(in_ram(a, 2), 1)) { uint16_t v; __builtin_memcpy(&v, gml_ram + (a & 0x3FFFFFFFu), 2); return __builtin_bswap16(v); } return (uint16_t)gml_ld_slow(a, 2); }
GML_INLINE uint32_t gml_ld32(uint32_t a) { if (__builtin_expect(in_ram(a, 4), 1)) { uint32_t v; __builtin_memcpy(&v, gml_ram + (a & 0x3FFFFFFFu), 4); return __builtin_bswap32(v); } return gml_ld_slow(a, 4); }
GML_INLINE uint64_t gml_ld64(uint32_t a) { if (__builtin_expect(in_ram(a, 8), 1)) { uint64_t v; __builtin_memcpy(&v, gml_ram + (a & 0x3FFFFFFFu), 8); return __builtin_bswap64(v); } return gml_ld64_slow(a); }
GML_INLINE float gml_ldf32(uint32_t a) { uint32_t w = gml_ld32(a); float f; __builtin_memcpy(&f, &w, 4); return f; }
GML_INLINE double gml_ldf64(uint32_t a) { uint64_t w = gml_ld64(a); double d; __builtin_memcpy(&d, &w, 8); return d; }
GML_INLINE void gml_st8(uint32_t a, uint8_t v) { if (__builtin_expect(in_ram(a, 1), 1)) { gml_ram[a & 0x3FFFFFFFu] = v; return; } gml_st_slow(a, v, 1); }
GML_INLINE void gml_st16(uint32_t a, uint16_t v) { if (__builtin_expect(in_ram(a, 2), 1)) { uint16_t s = __builtin_bswap16(v); __builtin_memcpy(gml_ram + (a & 0x3FFFFFFFu), &s, 2); return; } gml_st_slow(a, v, 2); }
GML_INLINE void gml_st32(uint32_t a, uint32_t v) { if (__builtin_expect(in_ram(a, 4), 1)) { uint32_t s = __builtin_bswap32(v); __builtin_memcpy(gml_ram + (a & 0x3FFFFFFFu), &s, 4); return; } gml_st_slow(a, v, 4); }
GML_INLINE void gml_st64(uint32_t a, uint64_t v) { if (__builtin_expect(in_ram(a, 8), 1)) { uint64_t s = __builtin_bswap64(v); __builtin_memcpy(gml_ram + (a & 0x3FFFFFFFu), &s, 8); return; } gml_st64_slow(a, v); }
GML_INLINE void gml_stf32(uint32_t a, float f) { uint32_t w; __builtin_memcpy(&w, &f, 4); gml_st32(a, w); }
GML_INLINE void gml_stf64(uint32_t a, double d) { uint64_t w; __builtin_memcpy(&w, &d, 8); gml_st64(a, w); }
GML_INLINE void gml_memcpy(uint32_t d, uint32_t s, uint32_t n) { if (__builtin_expect(in_ram(d, n) && in_ram(s, n), 1)) { __builtin_memcpy(gml_ram + (d & 0x3FFFFFFFu), gml_ram + (s & 0x3FFFFFFFu), n); return; } gml_memcpy_slow(d, s, n); }
GML_INLINE void gml_memmove(uint32_t d, uint32_t s, uint32_t n) { if (__builtin_expect(in_ram(d, n) && in_ram(s, n), 1)) { __builtin_memmove(gml_ram + (d & 0x3FFFFFFFu), gml_ram + (s & 0x3FFFFFFFu), n); return; } gml_memcpy_slow(d, s, n); }
GML_INLINE void gml_memset(uint32_t d, uint32_t v, uint32_t n) { if (__builtin_expect(in_ram(d, n), 1)) { __builtin_memset(gml_ram + (d & 0x3FFFFFFFu), (int)v, n); return; } gml_memset_slow(d, v, n); }
GML_INLINE void gml_set_r(uint32_t i, uint32_t v) { gml_regs[i] = v; }
GML_INLINE uint32_t gml_get_r(uint32_t i) { return gml_regs[i]; }
GML_INLINE void gml_set_f(uint32_t i, double d) { gml_fregs[2 * i] = d; gml_fregs[2 * i + 1] = d; }
GML_INLINE double gml_get_f(uint32_t i) { return gml_fregs[2 * i]; }
GML_INLINE void gml_set_cr6(_Bool on) { gml_cr[1] = (uint8_t)(on ? (gml_cr[1] | 2) : (gml_cr[1] & ~2)); }
