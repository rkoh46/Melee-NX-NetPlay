// Horizon back end of fixedmap.h. NOT VERIFIED ON HARDWARE YET: this is the one file to change if
// the kernel refuses any step. It follows what nx-hbloader does to place an NRO: heap pages are
// mapped at the wanted address with svcMapProcessCodeMemory, then given their final protection with
// svcSetProcessMemoryPermission (RWX is not allowed), and unmapped with svcUnmapProcessCodeMemory.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixedmap.h"
#include <switch.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RANGES 8
static struct Range { uintptr_t addr; size_t size; void* backing; VirtmemReservation* reservation; } g_ranges[MAX_RANGES];
static char g_error[320];

const char* fixedmap_error(void) { return g_error; }

void* fixedmap_stage(size_t size) {
  void* p = memalign(FIXEDMAP_PAGE, size);
  if (!p) { snprintf(g_error, sizeof g_error, "no heap for %zu KiB of staging pages", size >> 10); return NULL; }
  memset(p, 0, size);
  return p;
}

// 0 when [addr, addr+size) is unmapped.
static int range_is_free(uintptr_t addr, size_t size, const char* name) {
  for (u64 at = addr; at < addr + size;) {
    MemoryInfo mi; u32 page_info;
    Result rc = svcQueryMemory(&mi, &page_info, at);
    if (R_FAILED(rc)) { snprintf(g_error, sizeof g_error, "%s: svcQueryMemory(%lx) rc=%x", name, at, rc); return -1; }
    if (mi.type != MemType_Unmapped) {
      snprintf(g_error, sizeof g_error, "%s: %lx..%lx is in use (type %x). The address space is laid out "
               "randomly per process: restart the title that hosts the Homebrew Menu and try again",
               name, mi.addr, mi.addr + mi.size, mi.type & 0xff);
      return -1;
    }
    if (!mi.size) break;
    at = mi.addr + mi.size;
  }
  return 0;
}

int fixedmap_commit(uintptr_t addr, void* staged, size_t size, const char* name) {
  Handle proc = envGetOwnProcessHandle();
  struct Range* slot = NULL;
  for (int i = 0; i < MAX_RANGES; ++i) if (!g_ranges[i].size) { slot = &g_ranges[i]; break; }
  if (proc == INVALID_HANDLE) snprintf(g_error, sizeof g_error, "%s: no own-process handle (run from the Homebrew Menu)", name);
  else if (!slot) snprintf(g_error, sizeof g_error, "%s: too many ranges", name);
  else if ((addr | size | (uintptr_t)staged) & (FIXEDMAP_PAGE - 1)) snprintf(g_error, sizeof g_error, "%s: not page aligned", name);
  else if (range_is_free(addr, size, name) == 0) {
    armDCacheFlush(staged, size);           // the pages may hold code
    // Keep libnx's own address-space allocator (thread stacks, shared memory) out of the range.
    virtmemLock();
    VirtmemReservation* reservation = virtmemAddReservation((void*)addr, size);
    virtmemUnlock();
    Result rc = svcMapProcessCodeMemory(proc, addr, (u64)staged, size);
    if (R_SUCCEEDED(rc)) {
      slot->addr = addr; slot->size = size; slot->backing = staged; slot->reservation = reservation;
      return 0;
    }
    // The kernel only accepts addresses inside the ASLR region and outside the heap and alias
    // regions, mapped or not, so say where those are. A host title with a 36-bit address space
    // (ASLR region ending at 0x80000000) can never work: use the album applet or a 39-bit title.
    u64 aslr = 0, aslr_size = 0, heap = 0, heap_size = 0, alias = 0, alias_size = 0;
    svcGetInfo(&aslr, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0); svcGetInfo(&aslr_size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&heap, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0); svcGetInfo(&heap_size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&alias, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0); svcGetInfo(&alias_size, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
    snprintf(g_error, sizeof g_error, "%s: svcMapProcessCodeMemory(%lx, %zu KiB) rc=%x [aslr %lx+%lx heap %lx+%lx alias %lx+%lx]", name,
             (unsigned long)addr, size >> 10, rc, aslr, aslr_size, heap, heap_size, alias, alias_size);
    if (reservation) { virtmemLock(); virtmemRemoveReservation(reservation); virtmemUnlock(); }
  }
  free(staged);
  return -1;
}

int fixedmap_protect(uintptr_t addr, size_t size, int prot) {
  u32 perm = prot == FIXEDMAP_RX ? Perm_Rx : prot == FIXEDMAP_RW ? Perm_Rw : Perm_R;
  Result rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), addr, size, perm);
  if (R_FAILED(rc)) {
    snprintf(g_error, sizeof g_error, "svcSetProcessMemoryPermission(%lx, %zu KiB, %x) rc=%x", (unsigned long)addr, size >> 10, perm, rc);
    return -1;
  }
  if (prot == FIXEDMAP_RX) armICacheInvalidate((void*)addr, size);
  return 0;
}

static void release(struct Range* r) {
  if (!r->size) return;
  Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), r->addr, (u64)r->backing, r->size);
  if (R_FAILED(rc)) {
    // Leave the backing pages alone: they are still mapped away. Leaks until the process ends.
    snprintf(g_error, sizeof g_error, "svcUnmapProcessCodeMemory(%lx) rc=%x", (unsigned long)r->addr, rc);
  } else {
    free(r->backing);
  }
  if (r->reservation) { virtmemLock(); virtmemRemoveReservation(r->reservation); virtmemUnlock(); }
  memset(r, 0, sizeof *r);
}

void* fixedmap_backing(uintptr_t addr) {
  for (int i = 0; i < MAX_RANGES; ++i)
    if (g_ranges[i].size && addr >= g_ranges[i].addr && addr - g_ranges[i].addr < g_ranges[i].size)
      return (u8*)g_ranges[i].backing + (addr - g_ranges[i].addr);
  return NULL;
}

void fixedmap_release(uintptr_t addr) {
  for (int i = 0; i < MAX_RANGES; ++i) if (g_ranges[i].size && g_ranges[i].addr == addr) release(&g_ranges[i]);
}
void fixedmap_release_all(void) {
  for (int i = MAX_RANGES - 1; i >= 0; --i) release(&g_ranges[i]);
}
