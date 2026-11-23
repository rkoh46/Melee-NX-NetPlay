// Horizon back end of fixedmap.h. It follows what nx-hbloader does to place an NRO: heap pages are
// mapped at the wanted address with svcMapProcessCodeMemory, then given their final protection with
// svcSetProcessMemoryPermission (RWX is not allowed), and unmapped with svcUnmapProcessCodeMemory.
// Mapping, protecting and unmapping are verified on the console. The layout check and the relaunch
// at the end of the file are new and verified on paper only (see README, "Process layout").
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixedmap.h"
#include <switch.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RANGES 8
static struct Range { uintptr_t addr; size_t size; void* backing; VirtmemReservation* reservation; } g_ranges[MAX_RANGES];
static char g_error[400];
static char g_layout[400] = "layout: not checked (fixedmap_startup was not called)";
#define MAX_EARLY 4
static struct { uintptr_t addr; size_t size; } g_early[MAX_EARLY];   // ranges reserved by fixedmap_startup
static int g_relaunch_count;

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
      snprintf(g_error, sizeof g_error, "%s: %lx..%lx is in use (type %x). The system lays the address space out "
               "randomly for each process and this one cannot hold the game: close the title and start it again",
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
    VirtmemReservation* reservation = NULL;
    int reserved_early = 0;
    for (int i = 0; i < MAX_EARLY; ++i) if (g_early[i].size && addr >= g_early[i].addr && addr + size <= g_early[i].addr + g_early[i].size) reserved_early = 1;
    if (!reserved_early) {
      virtmemLock();
      reservation = virtmemAddReservation((void*)addr, size);
      virtmemUnlock();
    }
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

// ---- the process layout, and starting over when it is in the way --------------------------------
// What the kernel does (Atmosphere, KPageTableBase::InitializeForProcess and CanContain): a 39-bit
// process has the range 0x8000000..0x8000000000 for everything. The loader puts the title's code at
// a random 2 MiB step in it. The kernel then gives each of four regions (kernel map 64 GiB, alias
// 64 GiB, heap 8 GiB, stack 2 GiB) to the side of the code with more room left, and spreads the
// regions of one side over that side in random order with random gaps. svcMapProcessCodeMemory
// refuses any destination that touches the heap region or the alias region, whatever is mapped
// there (svcSetHeapSize changes the mapping, not the region). Memory inside those regions can only
// be heap-state or IPC memory, which no call can make executable, so the game image cannot use the
// pages that are already there either. The stack and kernel-map regions are allowed as long as
// nothing is mapped on the ranges.
#define RELAUNCH_MAGIC 0x4C52554Du     // "MURL"
#define RELAUNCH_LIMIT 4               // one launch in 70 is bad: four in a row is one in 20 million
#define MAX_ARGS 96
struct RelaunchHeader { u32 magic, count, argc, bytes; };
static u8 g_payload[0x1000];           // what goes to the next process: header, then the arguments, each ending in 0
static size_t g_payload_size;
static char g_restored[0x1000];
static char* g_restored_argv[MAX_ARGS + 1];
static void* g_obstacle;               // --simulate-bad-layout: our page on the first range
static uintptr_t g_obstacle_at;

static u64 info(InfoType type) { u64 v = 0; svcGetInfo(&v, type, CUR_PROCESS_HANDLE, 0); return v; }
static int overlaps(u64 a, u64 a_size, u64 b, u64 b_size) { return a_size && b_size && a < b + b_size && b < a + a_size; }

static void remove_obstacle(void) {
  if (!g_obstacle) return;
  if (R_SUCCEEDED(svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), g_obstacle_at, (u64)g_obstacle, FIXEDMAP_PAGE))) free(g_obstacle);
  g_obstacle = NULL;
}

// The arguments of the first launch, when this process was started by fixedmap_relaunch.
static void restore_arguments(int* argc, char*** argv) {
  AppletType type = appletGetAppletType();
  if (type != AppletType_Application && type != AppletType_SystemApplication) return;
  AppletStorage storage;
  if (R_FAILED(appletPopLaunchParameter(&storage, AppletLaunchParameterKind_UserChannel))) return;
  s64 size = 0;
  struct RelaunchHeader h;
  if (R_SUCCEEDED(appletStorageGetSize(&storage, &size)) && size >= (s64)sizeof h && size <= (s64)sizeof g_payload &&
      R_SUCCEEDED(appletStorageRead(&storage, 0, g_payload, (size_t)size))) {
    memcpy(&h, g_payload, sizeof h);
    if (h.magic == RELAUNCH_MAGIC && h.argc >= 1 && h.argc <= MAX_ARGS && h.bytes <= (u64)size - sizeof h && h.bytes && g_payload[sizeof h + h.bytes - 1] == 0) {
      memcpy(g_restored, g_payload + sizeof h, h.bytes);
      u32 n = 0;
      for (u32 at = 0; at < h.bytes && n < h.argc; at += (u32)strlen(g_restored + at) + 1) g_restored_argv[n++] = g_restored + at;
      if (n == h.argc) { g_restored_argv[n] = NULL; *argc = (int)n; *argv = g_restored_argv; g_relaunch_count = (int)h.count; }
    }
  }
  appletStorageClose(&storage);
}

int fixedmap_startup(int* argc, char*** argv, const uintptr_t* ranges, int count) {
  restore_arguments(argc, argv);
  int allow = 0, simulate = 0;
  size_t bytes = 0;
  int args_fit = *argc >= 1 && *argc <= MAX_ARGS;
  for (int i = 0; i < *argc; ++i) {
    const char* a = (*argv)[i];
    if (!strcmp(a, "--relaunch")) allow = 1;
    if (!strcmp(a, "--simulate-bad-layout") && i + 1 < *argc) simulate = atoi((*argv)[i + 1]);
    size_t n = strlen(a) + 1;
    if (bytes + n > sizeof g_payload - sizeof(struct RelaunchHeader)) { args_fit = 0; break; }
    memcpy(g_payload + sizeof(struct RelaunchHeader) + bytes, a, n); bytes += n;
  }
  if (args_fit) {
    struct RelaunchHeader h = {RELAUNCH_MAGIC, (u32)g_relaunch_count + 1, (u32)*argc, (u32)bytes};
    memcpy(g_payload, &h, sizeof h);
    g_payload_size = sizeof h + bytes;
  }

  const u64 aslr = info(InfoType_AslrRegionAddress), aslr_size = info(InfoType_AslrRegionSize);
  const u64 heap = info(InfoType_HeapRegionAddress), heap_size = info(InfoType_HeapRegionSize);
  const u64 alias = info(InfoType_AliasRegionAddress), alias_size = info(InfoType_AliasRegionSize);
  const u64 stack = info(InfoType_StackRegionAddress), stack_size = info(InfoType_StackRegionSize);
  int at = snprintf(g_layout, sizeof g_layout, "layout (launch %d): aslr %lx+%lx heap %lx+%lx alias %lx+%lx stack %lx+%lx: ", g_relaunch_count + 1,
                    aslr, aslr_size, heap, heap_size, alias, alias_size, stack, stack_size);

  if (simulate > g_relaunch_count && count > 0 && envGetOwnProcessHandle() != INVALID_HANDLE) {
    void* page = memalign(FIXEDMAP_PAGE, FIXEDMAP_PAGE);
    if (page && R_SUCCEEDED(svcMapProcessCodeMemory(envGetOwnProcessHandle(), ranges[0], (u64)page, FIXEDMAP_PAGE))) { g_obstacle = page; g_obstacle_at = ranges[0]; }
    else free(page);      // the layout is already bad by itself, or there is no handle
  }

  char why[200] = "";
  for (int i = 0; i < count && !why[0]; ++i) {
    const u64 addr = ranges[2 * i], size = ranges[2 * i + 1];
    if (addr < aslr || addr + size > aslr + aslr_size)
      snprintf(why, sizeof why, "%lx+%lx is outside the address range of this title (a 32-bit or 36-bit title cannot host the game at all)", addr, size);
    else if (overlaps(addr, size, heap, heap_size)) snprintf(why, sizeof why, "%lx+%lx lies in the heap region", addr, size);
    else if (overlaps(addr, size, alias, alias_size)) snprintf(why, sizeof why, "%lx+%lx lies in the alias region", addr, size);
    else if (range_is_free(addr, size, "range")) snprintf(why, sizeof why, "%.150s", g_error);
  }
  if (!why[0]) {
    virtmemLock();
    for (int i = 0; i < count && i < MAX_EARLY; ++i)
      if (virtmemAddReservation((void*)ranges[2 * i], ranges[2 * i + 1])) { g_early[i].addr = ranges[2 * i]; g_early[i].size = ranges[2 * i + 1]; }
    virtmemUnlock();
    snprintf(g_layout + at, sizeof g_layout - at, "the fixed ranges are free");
    return 0;
  }
  const AppletType type = appletGetAppletType();
  const int is_application = type == AppletType_Application || type == AppletType_SystemApplication;
  const int outside = strstr(why, "outside") != NULL;
  const char* cannot = outside ? "a new process of this title would be the same" :
                       !allow ? "not starting over by itself: --relaunch was not given (under the Homebrew Menu a restart would not come back here)" :
                       !is_application ? "only a title can start itself again, and this is an applet" :
                       !hosversionAtLeast(5, 0, 0) ? "the system is older than 5.0.0" :
                       !args_fit ? "the arguments do not fit in the restart message" :
                       g_relaunch_count >= RELAUNCH_LIMIT ? "started over four times already" : NULL;
  snprintf(g_layout + at, sizeof g_layout - at, "BLOCKED%s, %s", g_obstacle ? " (simulated)" : "", why);
  if (!cannot) return 1;
  snprintf(g_error, sizeof g_error, "this process cannot hold the game: %s%s; %s. Close the title and start it again", why,
           g_obstacle ? " (simulated)" : "", cannot);
  remove_obstacle();
  return -1;
}

const char* fixedmap_layout(void) { return g_layout; }
int fixedmap_relaunch_count(void) { return g_relaunch_count; }

int fixedmap_relaunch(void) {
  Result rc = g_payload_size ? appletRestartProgram(g_payload, g_payload_size) : MAKERESULT(Module_Libnx, LibnxError_BadInput);
  // Only reached when the system refused.
  snprintf(g_error, sizeof g_error, "the system refused to start the title again (rc=%x). Close the title and start it again", rc);
  remove_obstacle();
  return -1;
}

