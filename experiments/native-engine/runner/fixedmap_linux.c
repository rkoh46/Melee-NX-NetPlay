// Linux back end of fixedmap.h, for testing the loader and runner on aarch64 Linux. It imitates the
// Horizon rules: nothing is accessible at the fixed address until fixedmap_protect is called.
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "fixedmap.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define MAX_RANGES 8
static struct Range { uintptr_t addr; size_t size; } g_ranges[MAX_RANGES];
static char g_error[192];
const char* fixedmap_error(void) { return g_error; }

void* fixedmap_stage(size_t size) {
  void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { snprintf(g_error, sizeof g_error, "staging mmap: %s", strerror(errno)); return NULL; }
  return p;
}

int fixedmap_commit(uintptr_t addr, void* staged, size_t size, const char* name) {
  struct Range* slot = NULL;
  for (int i = 0; i < MAX_RANGES; ++i) if (!g_ranges[i].size) { slot = &g_ranges[i]; break; }
  void* p = slot ? mmap((void*)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) : MAP_FAILED;
  if (p != MAP_FAILED && (uintptr_t)p != addr) { munmap(p, size); p = MAP_FAILED; errno = EEXIST; }
  if (p == MAP_FAILED) {
    snprintf(g_error, sizeof g_error, "%s: cannot map %zu KiB at %lx: %s", name, size >> 10, (unsigned long)addr, slot ? strerror(errno) : "too many ranges");
    munmap(staged, size);
    return -1;
  }
  memcpy(p, staged, size);
  munmap(staged, size);
  mprotect(p, size, PROT_NONE);
  slot->addr = addr; slot->size = size;
  return 0;
}

int fixedmap_protect(uintptr_t addr, size_t size, int prot) {
  int p = prot == FIXEDMAP_RX ? PROT_READ | PROT_EXEC : prot == FIXEDMAP_RW ? PROT_READ | PROT_WRITE : PROT_READ;
  if (mprotect((void*)addr, size, p)) { snprintf(g_error, sizeof g_error, "mprotect(%lx): %s", (unsigned long)addr, strerror(errno)); return -1; }
  if (prot == FIXEDMAP_RX) __builtin___clear_cache((char*)addr, (char*)addr + size);
  return 0;
}

void* fixedmap_backing(uintptr_t addr) { (void)addr; return NULL; }

void fixedmap_release(uintptr_t addr) {
  for (int i = 0; i < MAX_RANGES; ++i) if (g_ranges[i].size && g_ranges[i].addr == addr) {
    munmap((void*)g_ranges[i].addr, g_ranges[i].size);
    g_ranges[i].size = 0;
  }
}
void fixedmap_release_all(void) {
  for (int i = 0; i < MAX_RANGES; ++i) if (g_ranges[i].size) fixedmap_release(g_ranges[i].addr);
}
