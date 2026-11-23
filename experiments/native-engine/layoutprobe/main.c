// Layout probe for the native-engine experiment (see ../README.md, "Process layout").
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Uses the runner's own fixedmap_horizon.c, so what is tested here is what the runner does:
//  - prints where this process has its regions and whether the game's fixed ranges can be mapped;
//  - when they can, maps all three the way the runner does, writes and reads them, and lets go;
//  - when they cannot (or with --simulate-bad-layout N, in the first N launches, by putting a page of
//    ours on the first range), reports it, and with --relaunch asks the system to start the title
//    again with the same arguments;
//  - --restart-anyway N starts over N times even with a good layout: under a title that always
//    loads this NRO it collects N+1 layouts in one go (how often is a launch bad?).
// A process started by a relaunch says so and shows the arguments it got back. Every launch adds
// its lines to sdmc:/switch/mu-layoutprobe.txt and to nxlink when there is one.
#include "../runner/fixedmap.h"
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* g_file;
static void say(const char* fmt, ...) {
  char line[512];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  printf("%s\n", line);
  if (g_file) { fprintf(g_file, "%s\n", line); fflush(g_file); }
  consoleUpdate(NULL);
}

static int map_and_check(const char* name, uintptr_t addr, size_t size, int code) {
  u32* staged = fixedmap_stage(size);
  if (!staged) { say("%s: %s", name, fixedmap_error()); return 0; }
  staged[0] = 0x52800540u; staged[1] = 0xD65F03C0u;          // mov w0, #42 ; ret
  if (fixedmap_commit(addr, staged, size, name)) { say("%s: %s", name, fixedmap_error()); return 0; }
  int ok = 1;
  if (code) {
    if (fixedmap_protect(addr, FIXEDMAP_PAGE, FIXEDMAP_RX)) { say("%s: %s", name, fixedmap_error()); ok = 0; }
    else { int got = ((int (*)(void))addr)(); say("%s: code at %lx returned %d (%s)", name, (unsigned long)addr, got, got == 42 ? "ok" : "WRONG"); ok = got == 42; }
  }
  const uintptr_t data = addr + (code ? FIXEDMAP_PAGE : 0);
  if (fixedmap_protect(data, addr + size - data, FIXEDMAP_RW)) { say("%s: %s", name, fixedmap_error()); ok = 0; }
  else {
    volatile u32* first = (volatile u32*)data; volatile u32* last = (volatile u32*)(addr + size - 4);
    *first = 0x4D454C45; *last = 0x4E583634;
    const int rw = *first == 0x4D454C45 && *last == 0x4E583634;
    say("%s: data %lx..%lx read/write %s", name, (unsigned long)data, (unsigned long)(addr + size), rw ? "ok" : "WRONG");
    ok = ok && rw;
  }
  return ok;
}

int main(int argc, char** argv) {
  const int launch_argc = argc;
  static const uintptr_t ranges[] = {0x80000000u, 0x04000000u, 0xE0000000u, 0x00100000u};
  const int layout = fixedmap_startup(&argc, &argv, ranges, 2);       // first: before any thread exists
  consoleInit(NULL);
  socketInitializeDefault();
  nxlinkStdio();
  g_file = fopen("sdmc:/switch/mu-layoutprobe.txt", "a");
  int restart_anyway = 0, allow = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--restart-anyway") && i + 1 < argc) restart_anyway = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--relaunch")) allow = 1;
  }
  say("---- mu-layoutprobe ----");
  say("applet type %d, system %u.%u.%u%s, own process handle %s, memory total %lu MiB", appletGetAppletType(), HOSVER_MAJOR(hosversionGet()),
      HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()), hosversionIsAtmosphere() ? " (Atmosphere)" : "",
      envGetOwnProcessHandle() != INVALID_HANDLE ? "yes" : "NO", ({ u64 v = 0; svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0); v >> 20; }));
  if (fixedmap_relaunch_count()) {
    char line[400] = ""; int at = 0;
    for (int i = 0; i < argc && at < (int)sizeof line - 1; ++i) at += snprintf(line + at, sizeof line - at, " [%s]", argv[i]);
    say("STARTED BY A RELAUNCH: number %d; the loader gave %d argument(s), the restart message gave back %d:%.380s", fixedmap_relaunch_count(), launch_argc, argc, line);
  } else say("first launch (no restart message), %d argument(s)", argc);
  say("%s", fixedmap_layout());

  int done = 0;
  if (layout == 0) {
    const int m1 = map_and_check("mem1 ", 0x80000000u, 0x02800000u, 0);
    const int m2 = map_and_check("image", 0x82800000u, 0x01800000u, 1);
    const int m3 = map_and_check("cache", 0xE0000000u, 0x00100000u, 0);
    fixedmap_release_all();
    say("RESULT layout good, mem1=%d image=%d cache=%d, after %d relaunch(es)", m1, m2, m3, fixedmap_relaunch_count());
    if (restart_anyway > fixedmap_relaunch_count()) {
      say("--restart-anyway: starting the title again (%d of %d)", fixedmap_relaunch_count() + 1, restart_anyway);
      done = 1;
    }
  } else if (layout > 0) {
    say("RESULT layout BLOCKED; starting the title again with the same arguments (relaunch %d)", fixedmap_relaunch_count() + 1);
    done = 1;
  } else say("RESULT layout BLOCKED and no relaunch: %s", fixedmap_error());

  if (done) {
    if (!allow && layout == 0) say("(note: --relaunch is not among the arguments; --restart-anyway restarts all the same)");
    if (g_file) { fclose(g_file); g_file = NULL; }
    svcSleepThread(1500000000ull);                           // let the lines be read and reach nxlink
    fixedmap_relaunch();                                     // does not come back when it works
    g_file = fopen("sdmc:/switch/mu-layoutprobe.txt", "a");
    say("RELAUNCH REFUSED: %s", fixedmap_error());
  }
  say("---- done ----");
  if (g_file) fclose(g_file);
  svcSleepThread(4000000000ull);
  socketExit();
  consoleExit(NULL);
  return 0;
}
