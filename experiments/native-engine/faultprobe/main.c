// Fault probe for the native-engine rollback snapshot (see ../README.md).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snapshot design keeps the game's memory read-only and learns which pages a frame writes from
// the faults. One run of this answers, on the console:
//   1. Can a range at 0x80000000 that has been writable go read-only again, and how?
//        A: svcSetMemoryPermission (the kernel source says yes: the pages are in a reprotectable state)
//        B: unmap + map again + svcSetProcessMemoryPermission (stays within the code-memory calls)
//      with timings for 40 MiB and for single pages, and how the kernel's memory blocks split and merge.
//   2. A write fault that is caught, has its page made writable, and is retried: round trip time,
//      with and without a 4 KiB copy in the handler.
//   3. Whether every register survives the retry (x2-x28, flags, q0-q31, fpsr, fpcr), over about
//      200,000 faults so that interrupts arriving during the handler are included.
//   4. What a file read into a read-only page does (the kernel writes the buffer, nothing faults).
// It uses the runner's own fixedmap_horizon.c, dirtytrack_horizon.c and dirtytrack_horizon.S.
// Sections 1A and 1B never fault; if the first fault in section 2 kills the program, the lines
// before it are already in the log. Output: nxlink and sdmc:/switch/mu-faultprobe.txt.
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runner/fixedmap.h"
#include "../runner/dirtytrack.h"

u64 mu_canary_store(u64* address, u64 seed);
extern volatile uint32_t mu_fault_last_desc, mu_fault_last_esr;
extern volatile uint64_t mu_fault_last_far, mu_fault_last_pc;

#define BASE 0x80000000ul
#define SIZE (40ul << 20)
#define PAGE 0x1000ul
#define NPAGES (SIZE / PAGE)

static FILE* g_file;
static void say(const char* fmt, ...) {
  char line[320];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  printf("%s\n", line); fflush(stdout);
  if (g_file) { fprintf(g_file, "%s\n", line); fflush(g_file); }
  consoleUpdate(NULL);
}
static double us_since(u64 t0) { return armTicksToNs(armGetSystemTick() - t0) / 1000.0; }

// How many memory blocks the kernel reports inside the range, and the permission of the first.
static int count_blocks(u32* first_perm, u32* first_type) {
  int blocks = 0;
  for (u64 at = BASE; at < BASE + SIZE;) {
    MemoryInfo mi; u32 page_info;
    if (R_FAILED(svcQueryMemory(&mi, &page_info, at)) || !mi.size) break;
    if (!blocks) { if (first_perm) *first_perm = mi.perm; if (first_type) *first_type = mi.type & 0xff; }
    ++blocks;
    at = mi.addr + mi.size;
  }
  return blocks;
}
static void say_blocks(const char* when) {
  u32 perm = 0, type = 0;
  int blocks = count_blocks(&perm, &type);
  say("  blocks %s: %d (first: type %x perm %x)", when, blocks, type, perm);
}

static u8 g_copy[PAGE];
static volatile u64 g_callback_calls;
static void callback_count(uintptr_t page) { (void)page; ++g_callback_calls; }
static void callback_copy(uintptr_t page) { memcpy(g_copy, (void*)page, PAGE); ++g_callback_calls; }

// ---- 1A: svcSetMemoryPermission both ways --------------------------------------------------------
static bool test_setperm(void) {
  say("-- 1A. svcSetMemoryPermission (read-only and back)");
  *(volatile u64*)BASE = 0xA5A5A5A5A5A5A5A5ull;
  Result rc_ro = 0, rc_rw = 0; double ro = 0, rw = 0;
  for (int i = 0; i < 10; ++i) {
    u64 t0 = armGetSystemTick(); Result a = svcSetMemoryPermission((void*)BASE, SIZE, Perm_R); ro += us_since(t0);
    if (i == 0) say_blocks("after whole range read-only");
    t0 = armGetSystemTick(); Result b = svcSetMemoryPermission((void*)BASE, SIZE, Perm_Rw); rw += us_since(t0);
    if (R_FAILED(a)) rc_ro = a;
    if (R_FAILED(b)) rc_rw = b;
  }
  say("40 MiB in one call x10: read-only %.1f us each (rc %x), read/write %.1f us each (rc %x)", ro / 10, rc_ro, rw / 10, rc_rw);
  bool ok = R_SUCCEEDED(rc_ro) && R_SUCCEEDED(rc_rw);
  if (ok && *(volatile u64*)BASE != 0xA5A5A5A5A5A5A5A5ull) { say("contents changed!"); ok = false; }
  if (!ok) { say("RESULT A (svcSetMemoryPermission): DOES NOT WORK"); return false; }

  static const struct { const char* name; size_t size, stride; int count; } cases[] = {
    {"1 MiB pieces", 1ul << 20, 1ul << 20, 40}, {"64 KiB pieces", 64ul << 10, 64ul << 10, 640},
    {"single pages, adjacent", PAGE, PAGE, 2048}, {"single pages, every 4th", PAGE, 4 * PAGE, 2048},
  };
  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
    Result bad = 0;
    u64 t0 = armGetSystemTick();
    for (int i = 0; i < cases[c].count; ++i) { Result rc = svcSetMemoryPermission((void*)(BASE + i * cases[c].stride), cases[c].size, Perm_R); if (R_FAILED(rc)) bad = rc; }
    double ro_us = us_since(t0) / cases[c].count;
    int blocks = count_blocks(NULL, NULL);
    t0 = armGetSystemTick();
    for (int i = 0; i < cases[c].count; ++i) { Result rc = svcSetMemoryPermission((void*)(BASE + i * cases[c].stride), cases[c].size, Perm_Rw); if (R_FAILED(rc)) bad = rc; }
    double rw_us = us_since(t0) / cases[c].count;
    say("%-24s x%-4d read-only %.2f us each, read/write %.2f us each, blocks while read-only %d, after %d%s", cases[c].name, cases[c].count,
        ro_us, rw_us, blocks, count_blocks(NULL, NULL), bad ? "  (SOME CALLS FAILED)" : "");
    if (bad) { say("  last failing rc=%x", bad); ok = false; }
  }
  // The pattern the rollback uses: everything read-only, then scattered single pages writable, then all writable.
  u64 t0 = armGetSystemTick();
  svcSetMemoryPermission((void*)BASE, SIZE, Perm_R);
  double all_ro = us_since(t0);
  t0 = armGetSystemTick();
  for (int i = 0; i < 1000; ++i) svcSetMemoryPermission((void*)(BASE + (size_t)i * 7 * PAGE), PAGE, Perm_Rw);
  double single = us_since(t0) / 1000;
  int split = count_blocks(NULL, NULL);
  t0 = armGetSystemTick();
  Result rc = svcSetMemoryPermission((void*)BASE, SIZE, Perm_Rw);
  double all_rw = us_since(t0);
  say("rollback pattern: all read-only %.1f us; 1000 scattered pages writable %.2f us each (%d blocks); all writable again %.1f us (rc %x, %d blocks)",
      all_ro, single, split, all_rw, rc, count_blocks(NULL, NULL));
  if (R_FAILED(rc)) ok = false;
  say("RESULT A (svcSetMemoryPermission): %s", ok ? "WORKS" : "PARTLY FAILED");
  return ok;
}

// ---- 1B: unmap, map again, svcSetProcessMemoryPermission -----------------------------------------
// Returns 1 when it works, 0 when it does not, -1 when it failed halfway and the range is gone.
static int test_remap(void) {
  say("-- 1B. remap (svcUnmapProcessCodeMemory + svcMapProcessCodeMemory), then svcSetProcessMemoryPermission");
  Handle proc = envGetOwnProcessHandle();
  void* backing = fixedmap_backing(BASE);
  *(volatile u64*)(BASE + 8) = 0x5A5A5A5A5A5A5A5Aull;
  int result = 1;
  for (int round = 0; round < 3 && result == 1; ++round) {
    u64 t0 = armGetSystemTick();
    Result rc = svcUnmapProcessCodeMemory(proc, BASE, (u64)backing, SIZE);
    double unmap_us = us_since(t0);
    if (R_FAILED(rc)) { say("unmap rc=%x (range still mapped)", rc); result = 0; break; }
    t0 = armGetSystemTick();
    rc = svcMapProcessCodeMemory(proc, BASE, (u64)backing, SIZE);
    double map_us = us_since(t0);
    if (R_FAILED(rc)) { say("map again rc=%x: THE RANGE IS GONE, the rest of the probe is skipped", rc); return -1; }
    if (round == 0) say_blocks("right after mapping again");
    t0 = armGetSystemTick();
    rc = svcSetProcessMemoryPermission(proc, BASE, SIZE, Perm_R);
    double ro_us = us_since(t0);
    if (R_FAILED(rc)) { say("read-only rc=%x", rc); result = 0; }
    bool kept = R_SUCCEEDED(rc) && *(volatile u64*)(BASE + 8) == 0x5A5A5A5A5A5A5A5Aull;
    // Single pages writable (the one direction this call allows), then the rest.
    Result bad = 0;
    t0 = armGetSystemTick();
    for (int i = 0; i < 1000; ++i) { Result r = svcSetProcessMemoryPermission(proc, BASE + (size_t)i * 7 * PAGE, PAGE, Perm_Rw); if (R_FAILED(r)) bad = r; }
    double single = us_since(t0) / 1000;
    int split = count_blocks(NULL, NULL);
    // Everything writable again: page runs that are still read-only, one call each.
    t0 = armGetSystemTick();
    int calls = 0;
    for (u64 at = BASE; at < BASE + SIZE;) {
      MemoryInfo mi; u32 page_info;
      if (R_FAILED(svcQueryMemory(&mi, &page_info, at)) || !mi.size) break;
      u64 end = mi.addr + mi.size; if (end > BASE + SIZE) end = BASE + SIZE;
      if ((mi.perm & Perm_Rw) != Perm_Rw) { Result r = svcSetProcessMemoryPermission(proc, at, end - at, Perm_Rw); if (R_FAILED(r)) bad = r; ++calls; }
      at = end;
    }
    double rest = us_since(t0);
    say("round %d: unmap %.1f us, map %.1f us, 40 MiB read-only %.1f us, contents %s; 1000 single pages writable %.2f us each (%d blocks)%s; "
        "all writable again %.1f us in %d calls (%d blocks)", round, unmap_us, map_us, ro_us, kept ? "kept" : "NOT KEPT", single, split,
        bad ? " SOME FAILED" : "", rest, calls, count_blocks(NULL, NULL));
    if (bad) { say("  last failing rc=%x", bad); result = 0; }
    if (!kept) result = 0;
  }
  // Leave the range fully writable whatever happened, and make sure it is before anything writes to it.
  bool writable = true;
  for (u64 at = BASE; at < BASE + SIZE;) {
    MemoryInfo mi; u32 page_info;
    if (R_FAILED(svcQueryMemory(&mi, &page_info, at)) || !mi.size) { writable = false; break; }
    u64 end = mi.addr + mi.size; if (end > BASE + SIZE) end = BASE + SIZE;
    if ((mi.perm & Perm_Rw) != Perm_Rw && R_FAILED(svcSetProcessMemoryPermission(proc, at, end - at, Perm_Rw))) writable = false;
    at = end;
  }
  say("RESULT B (remap): %s", result == 1 ? "WORKS" : "DOES NOT WORK");
  if (!writable) { say("the range could not be made writable again: the rest of the probe is skipped"); return -1; }
  return result;
}

// ---- 2: fault round trip -------------------------------------------------------------------------
static u64 time_faults(const char* name, DirtyTrackFirstWrite callback, int count, int stride, int rounds) {
  u64 handled = 0;
  dirtytrack_install(callback);
  for (int round = 0; round < rounds; ++round) {
    u64 t0 = armGetSystemTick();
    if (dirtytrack_protect(BASE, SIZE)) { say("%s: protect failed: %s", name, dirtytrack_error()); return handled; }
    double protect_us = us_since(t0);
    u64 faults0 = dirtytrack_faults();
    t0 = armGetSystemTick();
    for (int i = 0; i < count; ++i) *(volatile u64*)(BASE + (size_t)i * stride * PAGE) = 0x1234567800000000ull + i;
    double us = us_since(t0);
    u64 faults = dirtytrack_faults() - faults0;
    handled += faults;
    int wrong = 0;
    for (int i = 0; i < count; ++i) if (*(volatile u64*)(BASE + (size_t)i * stride * PAGE) != 0x1234567800000000ull + i) ++wrong;
    t0 = armGetSystemTick();
    for (int i = 0; i < count; ++i) *(volatile u64*)(BASE + (size_t)i * stride * PAGE) = 0x1234567800000000ull + i;
    double base_us = us_since(t0);
    int blocks = count_blocks(NULL, NULL);
    t0 = armGetSystemTick();
    if (dirtytrack_release(BASE, SIZE)) say("%s: release failed: %s", name, dirtytrack_error());
    double release_us = us_since(t0);
    say("fault %-22s round %d: %d writes, %lu faults, %.2f us per fault (without faults %.3f us), wrong values %d | protect 40 MiB %.1f us, "
        "%d blocks after the faults, release %.1f us, %d blocks after", name, round, count, (unsigned long)faults, us / count, base_us / count,
        wrong, protect_us, blocks, release_us, count_blocks(NULL, NULL));
  }
  return handled;
}

int main(int argc, char** argv) {
  consoleInit(NULL);
  socketInitializeDefault();
  nxlinkStdio();
  g_file = fopen("sdmc:/switch/mu-faultprobe.txt", "a");
  say("---- mu-faultprobe 2 ----");
  void* staged = fixedmap_stage(SIZE);
  bool mapped = staged && !fixedmap_commit(BASE, staged, SIZE, "probe range") && !fixedmap_protect(BASE, SIZE, FIXEDMAP_RW);
  if (!mapped) say("cannot map 40 MiB at %lx: %s", BASE, fixedmap_error());
  else {
    say("mapped 40 MiB at %lx like the engine's main memory, read/write", BASE);
    say_blocks("at the start");
    bool a_works = test_setperm();
    int b_works = test_remap();
    if (b_works < 0) mapped = false;

    if (mapped && (a_works || b_works == 1)) {
      const int method = a_works ? DIRTYTRACK_SETPERM : DIRTYTRACK_REMAP;
      dirtytrack_set_method(method);
      say("-- 2. write fault round trip, protecting with method %s", method == DIRTYTRACK_SETPERM ? "A (svcSetMemoryPermission)" : "B (remap)");
      say("   (if nothing follows this line, the first fault ended the program: exceptions do not reach the NRO, or the retry failed)");
      u64 handled = time_faults("no copy, adjacent", callback_count, 1024, 1, 3);
      handled += time_faults("no copy, every 4th", callback_count, 1024, 4, 3);
      handled += time_faults("4 KiB copy, every 4th", callback_copy, 1024, 4, 3);
      say("handler calls seen by the callback: %lu, faults handled: %lu", (unsigned long)g_callback_calls, (unsigned long)handled);
      if (a_works && b_works == 1) {
        dirtytrack_set_method(DIRTYTRACK_REMAP);
        time_faults("method B, every 4th", callback_copy, 1024, 4, 2);
        dirtytrack_set_method(method);
      }

      say("-- 3. registers across the retry");
      dirtytrack_install(callback_copy);
      u64 mask = 0, bad = 0, total = 0, without = 0;
      for (int i = 0; i < 4096; ++i) without |= mu_canary_store((u64*)(BASE + (size_t)i * PAGE), 0x1000000 + i);
      say("checker on writable pages (no faults): mask %lx (must be 0)", (unsigned long)without);
      u64 faults0 = dirtytrack_faults();
      u64 t0 = armGetSystemTick();
      for (int round = 0; round < 20 && !without; ++round) {
        if (dirtytrack_protect(BASE, SIZE)) { say("protect failed: %s", dirtytrack_error()); break; }
        for (size_t i = 0; i < NPAGES; ++i) {
          u64 m = mu_canary_store((u64*)(BASE + i * PAGE), 0x5A5A0000 + total);
          mask |= m; if (m) ++bad; ++total;
        }
        dirtytrack_release(BASE, SIZE);
      }
      u64 faulted = dirtytrack_faults() - faults0;
      say("%lu stores, %lu of them faulted, in %.1f s: %lu with a damaged register, mask %lx", (unsigned long)total, (unsigned long)faulted,
          us_since(t0) / 1e6, (unsigned long)bad, (unsigned long)mask);
      say("  mask bits: 2-28 = x2-x28, 29 = flags, 30/31 = vector low/high halves, 32 = fpsr, 33 = fpcr");
      if (without) say("RESULT registers UNTESTED (the checker itself reports a difference without any fault)");
      else if (!faulted) say("RESULT registers UNTESTED (no store faulted)");
      else say("RESULT registers %s", bad ? "NOT PRESERVED" : "PRESERVED");

      say("-- 4. file read into a read-only page");
      const char* path = "sdmc:/switch/mu-faultprobe.tmp";
      FILE* out = fopen(path, "wb");
      if (out) { char text[64]; memset(text, 'Q', sizeof text); fwrite(text, 1, sizeof text, out); fclose(out); }
      FILE* in = out ? fopen(path, "rb") : NULL;
      if (!in) say("cannot create the test file");
      else {
        memset((void*)BASE, 0xEE, 64);
        dirtytrack_protect(BASE, PAGE);
        u64 before = dirtytrack_faults();
        setvbuf(in, NULL, _IONBF, 0);              // unbuffered: the read goes straight to the given buffer
        size_t n = fread((void*)BASE, 1, 32, in);
        u8 first = *(volatile u8*)BASE;
        say("fread of 32 bytes into a read-only page returned %zu, faults %lu, first byte now %02x (%s)", n,
            (unsigned long)(dirtytrack_faults() - before), first, first == 0xEE ? "buffer untouched" : "buffer written");
        say("  the runner does not rely on this: it makes destination pages writable before a disc read");
        dirtytrack_release(BASE, PAGE);
        fclose(in);
        remove(path);
      }
      if (mu_fault_last_desc)
        say("an unhandled exception was recorded: desc %x esr %x far %lx pc %lx", mu_fault_last_desc, mu_fault_last_esr,
            (unsigned long)mu_fault_last_far, (unsigned long)mu_fault_last_pc);
    } else if (mapped) {
      say("-- 2-4 UNTESTED: neither method can make the range read-only");
    }
  }
  if (mapped) fixedmap_release_all();
  say("---- done ----");
  if (g_file) fclose(g_file);
  svcSleepThread(3000000000ull);
  socketExit();
  consoleExit(NULL);
  return 0;
}
