// Horizon back end of dirtytrack.h.
//
// Protection (checked against the kernel source, Atmosphere's mesosphere, kern_k_page_table_base.cpp):
// svcSetProcessMemoryPermission only accepts pages in a "code" state and turns them into "code data"
// when it makes them writable, so it cannot make our read/write pages read-only again (rc 0xd401 on
// the console). svcSetMemoryPermission accepts any state that can be reprotected, which "code data"
// is, and changes the permission both ways. That is DIRTYTRACK_SETPERM, the default. DIRTYTRACK_REMAP
// is the alternative that stays within the code-memory calls (see dirtytrack.h).
//
// Faults: a write to a read-only page raises a data abort; the kernel enters the program's exception
// entry (dirtytrack_horizon.S replaces libnx's), which calls mu_fault_handler below. Resuming takes
// two trips through the kernel, explained in the assembly file.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dirtytrack.h"
#include "fixedmap.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>

#define PAGES ((DIRTYTRACK_END - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE)
static uint8_t g_protected[PAGES];
static DirtyTrackFirstWrite g_callback;
static volatile uint64_t g_faults;
static Handle g_process = INVALID_HANDLE;
static int g_method = DIRTYTRACK_SETPERM;
static char g_error[200];

// Shared with dirtytrack_horizon.S.
ThreadExceptionFrameA64 mu_fault_frame;        // the interrupted code's x0-x8, lr, sp, pc, pstate
volatile uint32_t mu_fault_resume_pending;     // set between the two trips
void mu_fault_resume_stub(void);
// What the last unhandled exception looked like, for a log line if the program survives it.
volatile uint32_t mu_fault_last_desc, mu_fault_last_esr;
volatile uint64_t mu_fault_last_far, mu_fault_last_pc;

const char* dirtytrack_error(void) { return g_error; }
uint64_t dirtytrack_faults(void) { return g_faults; }
void dirtytrack_set_method(int method) { g_method = method; }
int dirtytrack_is_protected(uintptr_t page) {
  return page >= DIRTYTRACK_BASE && page < DIRTYTRACK_END && g_protected[(page - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE];
}

static Result make_writable(uintptr_t addr, size_t size) {
  return g_method == DIRTYTRACK_REMAP ? svcSetProcessMemoryPermission(g_process, addr, size, Perm_Rw)
                                      : svcSetMemoryPermission((void*)addr, size, Perm_Rw);
}

static int first_write(uintptr_t page) {
  if (!dirtytrack_is_protected(page)) return -1;
  g_protected[(page - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE] = 0;
  if (g_callback) g_callback(page);
  if (R_FAILED(make_writable(page, DIRTYTRACK_PAGE))) return -1;
  ++g_faults;
  return 0;
}

// Called by __libnx_exception_entry (dirtytrack_horizon.S) in exception context, on its own stack,
// with every register the C compiler may clobber already saved. Returns 0 when the write can be
// retried: the frame then leads to mu_fault_resume_stub, which finishes the job.
int mu_fault_handler(u32 desc, ThreadExceptionFrameA64* frame) {
  if (first_write(frame->far & ~(DIRTYTRACK_PAGE - 1)) != 0) {
    mu_fault_last_desc = desc; mu_fault_last_esr = frame->esr; mu_fault_last_far = frame->far; mu_fault_last_pc = frame->elr_el1;
    return 1;
  }
  mu_fault_frame = *frame;
  mu_fault_resume_pending = 1;
  frame->elr_el1 = (u64)(uintptr_t)&mu_fault_resume_stub;
  return 0;
}

int dirtytrack_install(DirtyTrackFirstWrite callback) {
  g_callback = callback;
  g_process = envGetOwnProcessHandle();
  if (g_process == INVALID_HANDLE) { snprintf(g_error, sizeof g_error, "no own-process handle"); return -1; }
  return 0;
}

static int check_range(uintptr_t addr, size_t size) {
  if ((addr | size) & (DIRTYTRACK_PAGE - 1) || addr < DIRTYTRACK_BASE || addr + size > DIRTYTRACK_END) {
    snprintf(g_error, sizeof g_error, "range %lx+%zx is not page aligned inside the tracked window", (unsigned long)addr, size);
    return -1;
  }
  return 0;
}

int dirtytrack_protect(uintptr_t addr, size_t size) {
  if (check_range(addr, size)) return -1;
  Result rc;
  if (g_method == DIRTYTRACK_REMAP) {
    void* backing = fixedmap_backing(addr);
    if (!backing) { snprintf(g_error, sizeof g_error, "range %lx is not a fixedmap range", (unsigned long)addr); return -1; }
    rc = svcUnmapProcessCodeMemory(g_process, addr, (u64)backing, size);
    if (R_FAILED(rc)) { snprintf(g_error, sizeof g_error, "remap %lx+%zx: unmap rc=%x (range still mapped)", (unsigned long)addr, size, rc); return -1; }
    rc = svcMapProcessCodeMemory(g_process, addr, (u64)backing, size);
    if (R_FAILED(rc)) { snprintf(g_error, sizeof g_error, "remap %lx+%zx: map rc=%x (RANGE IS NOW UNMAPPED)", (unsigned long)addr, size, rc); return -1; }
    rc = svcSetProcessMemoryPermission(g_process, addr, size, Perm_R);
    if (R_FAILED(rc)) { snprintf(g_error, sizeof g_error, "remap %lx+%zx: read-only rc=%x", (unsigned long)addr, size, rc); return -1; }
  } else {
    rc = svcSetMemoryPermission((void*)addr, size, Perm_R);
    if (R_FAILED(rc)) { snprintf(g_error, sizeof g_error, "svcSetMemoryPermission(%lx, %zx, r--) rc=%x", (unsigned long)addr, size, rc); return -1; }
  }
  memset(g_protected + (addr - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE, 1, size / DIRTYTRACK_PAGE);
  return 0;
}

// Writable again. Done per run of pages that are still read-only: with DIRTYTRACK_REMAP the call
// refuses a range that contains pages it already made writable.
int dirtytrack_release(uintptr_t addr, size_t size) {
  if (check_range(addr, size)) return -1;
  const size_t first = (addr - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE, end = first + size / DIRTYTRACK_PAGE;
  int result = 0;
  for (size_t i = first; i < end;) {
    if (!g_protected[i]) { ++i; continue; }
    size_t j = i;
    while (j < end && g_protected[j]) ++j;
    Result rc = make_writable(DIRTYTRACK_BASE + i * DIRTYTRACK_PAGE, (j - i) * DIRTYTRACK_PAGE);
    if (R_FAILED(rc)) { snprintf(g_error, sizeof g_error, "release %lx+%zx rc=%x", (unsigned long)(DIRTYTRACK_BASE + i * DIRTYTRACK_PAGE), (j - i) * DIRTYTRACK_PAGE, rc); result = -1; }
    else memset(g_protected + i, 0, j - i);
    i = j;
  }
  return result;
}

void dirtytrack_prewrite(void* addr, size_t size) {
  if (!size) return;
  const uintptr_t first = (uintptr_t)addr & ~(DIRTYTRACK_PAGE - 1), last = ((uintptr_t)addr + size - 1) & ~(DIRTYTRACK_PAGE - 1);
  for (uintptr_t page = first; page <= last; page += DIRTYTRACK_PAGE)
    if (dirtytrack_is_protected(page)) first_write(page);
}
