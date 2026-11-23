// The C library functions the game image imports, bound to this program's own C library (newlib on
// Horizon, glibc in the Linux test build). The game is an AArch64 Linux shared object; AAPCS64 is
// the calling convention on both sides, va_list has the same layout, long and size_t are 64-bit,
// and FILE* is only ever passed back to the library that produced it.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GCC's stack protector on AArch64 Linux reads a global guard (not thread-local storage).
uintptr_t mu_stack_chk_guard = 0x595e9fbd94fda766ull;
static void mu_stack_chk_fail(void) {
  fputs("game image: stack protector check failed\n", stderr);
  abort();
}

struct MuImport { const char* name; void* address; };
#define F(f) { #f, (void*)&f }
static const struct MuImport table[] = {
  F(malloc), F(free), F(memchr), F(memcmp), F(memcpy), F(memmove), F(memset),
  F(strcat), F(strchr), F(strcmp), F(strcpy), F(strlen), F(strncmp), F(strncpy), F(strrchr),
  F(strtoul), F(atoi), F(getenv),
  F(fopen), F(fclose), F(fflush), F(fwrite), F(fprintf), F(printf), F(snprintf), F(sprintf),
  F(vsnprintf), F(vsprintf),
  // fabs and fabsf clear the sign bit; sqrtf is correctly rounded by IEEE 754. No other maths comes
  // from the C library: the simulation's own routines are inside the image.
  F(fabs), F(fabsf), F(sqrtf),
  { "__stack_chk_guard", &mu_stack_chk_guard },
  { "__stack_chk_fail", (void*)&mu_stack_chk_fail },
};

void* mu_import(const char* name) {
  for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i)
    if (!strcmp(name, table[i].name)) return table[i].address;
  return NULL;
}
