// Write tracking for rollback snapshots: pages are made read-only, the first write to one is
// caught, reported, and the page becomes writable again so the write can be retried.
// Two back ends: dirtytrack_linux.c (mprotect + SIGSEGV) and dirtytrack_horizon.c with
// dirtytrack_horizon.S (svcSetProcessMemoryPermission + the user exception entry).
// Only pages inside 0x80000000..0x84000000 (main memory and the game image) can be tracked.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DIRTYTRACK_BASE 0x80000000ul
#define DIRTYTRACK_END  0x84000000ul
#define DIRTYTRACK_PAGE 0x1000ul

// Called from inside the fault handler, before the page becomes writable: it must not allocate,
// print or take locks. The page still holds the bytes it had when it was protected.
typedef void (*DirtyTrackFirstWrite)(uintptr_t page);

// How pages are made read-only. Horizon only (Linux ignores it):
//   DIRTYTRACK_SETPERM  svcSetMemoryPermission both ways. The kernel allows it on any state that can
//                       be reprotected, which includes the read/write "code data" state our pages
//                       are in once fixedmap_protect has made them writable.
//   DIRTYTRACK_REMAP    unmap and map the range again (svcUnmapProcessCodeMemory, svcMapProcessCodeMemory:
//                       the contents stay, the pages are fresh "code" again), set it read-only with
//                       svcSetProcessMemoryPermission, and make single pages writable with the same call,
//                       which is the one direction that call allows. The range is unmapped for a moment:
//                       nothing may touch it meanwhile.
enum { DIRTYTRACK_SETPERM = 0, DIRTYTRACK_REMAP = 1 };
void dirtytrack_set_method(int method);
// Installs the fault handler. Returns 0 on success.
int dirtytrack_install(DirtyTrackFirstWrite callback);
// Makes a page-aligned range read-only and tracked / writable and untracked. 0 on success.
int dirtytrack_protect(uintptr_t addr, size_t size);
int dirtytrack_release(uintptr_t addr, size_t size);
// Is this page currently read-only and tracked?
int dirtytrack_is_protected(uintptr_t page);
// Makes sure the range can be written by code that cannot fault (the kernel filling a read
// buffer): every tracked page in it goes through the first-write path now.
void dirtytrack_prewrite(void* addr, size_t size);
uint64_t dirtytrack_faults(void);
const char* dirtytrack_error(void);

#ifdef __cplusplus
}
#endif
