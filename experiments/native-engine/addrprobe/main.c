// Address probe for the native-engine experiment (see ../README.md).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native engine needs three fixed ranges: the console's main memory at 0x80000000 (40 MiB), the
// game image right after it at 0x82800000 (24 MiB, code and data) and the locked cache at 0xE0000000.
// A Homebrew program is loaded at a random address, so this asks Horizon whether those ranges are
// free and whether heap pages can be mapped there (data read/write, code read/execute), then times
// a 43 MiB copy, the size of one rollback snapshot. Output goes to nxlink and sdmc:/switch/mu-addrprobe.txt.
#include <switch.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* g_file;
static void say(const char* fmt, ...) {
  char line[256];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  printf("%s\n", line);
  if (g_file) { fprintf(g_file, "%s\n", line); fflush(g_file); }
  consoleUpdate(NULL);
}

static u64 info(InfoType type) {
  u64 value = 0;
  svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0);
  return value;
}

// Prints every block in [start, end) that is not free. Returns true when the whole range is free.
static bool range_free(const char* name, u64 start, u64 end) {
  bool free_all = true;
  for (u64 at = start; at < end;) {
    MemoryInfo mi; u32 page;
    if (R_FAILED(svcQueryMemory(&mi, &page, at))) { say("%s: query failed at %lx", name, at); return false; }
    if (mi.type != MemType_Unmapped) {
      free_all = false;
      say("%s: in use %lx..%lx type %x perm %x", name, mi.addr, mi.addr + mi.size, mi.type & 0xff, mi.perm);
    }
    at = mi.addr + mi.size;
    if (mi.size == 0) break;
  }
  say("%s %lx..%lx: %s", name, start, end, free_all ? "free" : "NOT free");
  return free_all;
}

// What was mapped, so it can be taken down before exit: leaving heap pages mapped as code makes the
// Homebrew Menu crash when the program returns to it.
static struct { u64 dst; u8* src; size_t size; } g_maps[4];
static int g_map_count;

// Maps `size` heap bytes at `dst`. With `code`, the first page holds a function and is made executable.
static bool map_at(const char* name, u64 dst, size_t size, bool code) {
  Handle proc = envGetOwnProcessHandle();
  u8* src = memalign(0x1000, size);
  if (!src) { say("%s: no heap for %zu MiB", name, size >> 20); return false; }
  memset(src, 0, size);
  if (code) {
    const u32 fn[] = {0x52800540u, 0xD65F03C0u};   // mov w0, #42 ; ret
    memcpy(src, fn, sizeof fn);
    armDCacheFlush(src, 0x1000);
  }
  Result rc = svcMapProcessCodeMemory(proc, dst, (u64)src, size);
  if (R_FAILED(rc)) { say("%s: map failed rc=%x", name, rc); free(src); return false; }
  g_maps[g_map_count].dst = dst; g_maps[g_map_count].src = src; g_maps[g_map_count++].size = size;
  bool ok = true;
  if (code) {
    rc = svcSetProcessMemoryPermission(proc, dst, 0x1000, Perm_Rx);
    if (R_FAILED(rc)) { say("%s: set r-x failed rc=%x", name, rc); ok = false; }
    else {
      armICacheInvalidate((void*)dst, 0x1000);
      int got = ((int (*)(void))dst)();
      say("%s: code at %lx returned %d (%s)", name, dst, got, got == 42 ? "ok" : "WRONG");
      ok = got == 42;
    }
    dst += 0x1000; size -= 0x1000;
  }
  rc = svcSetProcessMemoryPermission(proc, dst, size, Perm_Rw);
  if (R_FAILED(rc)) { say("%s: set rw- failed rc=%x", name, rc); ok = false; }
  else {
    volatile u32* first = (volatile u32*)dst;
    volatile u32* last = (volatile u32*)(dst + size - 4);
    *first = 0x4D454C45; *last = 0x4E583634;
    bool rw = *first == 0x4D454C45 && *last == 0x4E583634;
    say("%s: data %lx..%lx read/write %s", name, dst, dst + size, rw ? "ok" : "WRONG");
    ok = ok && rw;
  }
  return ok;   // stays mapped until main() ends: the three ranges must coexist
}

static void time_copy(size_t size) {
  u8* a = memalign(0x1000, size);
  u8* b = memalign(0x1000, size);
  if (!a || !b) { say("copy: no heap"); return; }
  memset(a, 0x5A, size); memset(b, 0, size);
  for (int i = 0; i < 5; ++i) {
    a[i * 4096] ^= 1;
    u64 t0 = armGetSystemTick();
    memcpy(b, a, size);
    u64 ns = armTicksToNs(armGetSystemTick() - t0);
    say("copy %zu MiB run %d: %.2f ms", size >> 20, i, ns / 1e6);
  }
  free(a); free(b);
}

int main(int argc, char** argv) {
  consoleInit(NULL);
  socketInitializeDefault();
  nxlinkStdio();
  g_file = fopen("sdmc:/switch/mu-addrprobe.txt", "a");
  say("---- mu-addrprobe ----");
  say("own process handle: %s, applet type %d", envGetOwnProcessHandle() != INVALID_HANDLE ? "yes" : "NO", appletGetAppletType());
  say("main() at %p, heap sample %p, stack %p", (void*)main, malloc(16), (void*)&argc);
  say("alias %lx +%lx", info(InfoType_AliasRegionAddress), info(InfoType_AliasRegionSize));
  say("heap  %lx +%lx", info(InfoType_HeapRegionAddress), info(InfoType_HeapRegionSize));
  say("aslr  %lx +%lx", info(InfoType_AslrRegionAddress), info(InfoType_AslrRegionSize));
  say("stack %lx +%lx", info(InfoType_StackRegionAddress), info(InfoType_StackRegionSize));
  say("memory total %lu MiB, used %lu MiB", info(InfoType_TotalMemorySize) >> 20, info(InfoType_UsedMemorySize) >> 20);

  bool f1 = range_free("mem1 ", 0x80000000ul, 0x82800000ul);
  bool f2 = range_free("image", 0x82800000ul, 0x84000000ul);
  bool f3 = range_free("cache", 0xE0000000ul, 0xE0100000ul);
  bool m1 = f1 && map_at("mem1 ", 0x80000000ul, 0x2800000, false);
  bool m2 = f2 && map_at("image", 0x82800000ul, 0x1800000, true);
  bool m3 = f3 && map_at("cache", 0xE0000000ul, 0x100000, false);
  say("RESULT mem1=%d image=%d cache=%d", m1, m2, m3);
  time_copy(43u << 20);
  for (int i = 0; i < g_map_count; ++i) {
    Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), g_maps[i].dst, (u64)g_maps[i].src, g_maps[i].size);
    if (R_FAILED(rc)) say("unmap %lx failed rc=%x", g_maps[i].dst, rc); else free(g_maps[i].src);
  }
  say("---- done ----");
  if (g_file) fclose(g_file);
  svcSleepThread(3000000000ull);
  socketExit();
  consoleExit(NULL);
  return 0;
}
