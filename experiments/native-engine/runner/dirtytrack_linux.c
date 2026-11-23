// Linux back end of dirtytrack.h: mprotect and a SIGSEGV handler.
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "dirtytrack.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#define PAGES ((DIRTYTRACK_END - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE)
static uint8_t g_protected[PAGES];
static DirtyTrackFirstWrite g_callback;
static volatile uint64_t g_faults;
static char g_error[160];
static struct sigaction g_previous;

const char* dirtytrack_error(void) { return g_error; }
uint64_t dirtytrack_faults(void) { return g_faults; }
int dirtytrack_is_protected(uintptr_t page) {
  return page >= DIRTYTRACK_BASE && page < DIRTYTRACK_END && g_protected[(page - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE];
}

static int first_write(uintptr_t page) {
  if (!dirtytrack_is_protected(page)) return -1;
  g_protected[(page - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE] = 0;
  if (g_callback) g_callback(page);
  if (mprotect((void*)page, DIRTYTRACK_PAGE, PROT_READ | PROT_WRITE)) return -1;
  ++g_faults;
  return 0;
}

static void on_fault(int sig, siginfo_t* info, void* context) {
  const int saved = errno;
  if (first_write((uintptr_t)info->si_addr & ~(DIRTYTRACK_PAGE - 1)) == 0) { errno = saved; return; }   // retry the write
  // Not ours: say where (dprintf is good enough for a process that is about to die), then let it
  // fault again with the previous disposition.
  dprintf(2, "dirtytrack: unhandled fault at address %p, pc %p, lr %p\n", info->si_addr,
          (void*)((ucontext_t*)context)->uc_mcontext.pc, (void*)((ucontext_t*)context)->uc_mcontext.regs[30]);
  sigaction(sig, &g_previous, NULL);
}

void dirtytrack_set_method(int method) { (void)method; }
int dirtytrack_install(DirtyTrackFirstWrite callback) {
  g_callback = callback;
  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_sigaction = on_fault;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);
  static int installed;      // a second install must not record this handler as the "previous" one
  if (sigaction(SIGSEGV, &action, installed ? NULL : &g_previous)) { snprintf(g_error, sizeof g_error, "sigaction: %s", strerror(errno)); return -1; }
  installed = 1;
  return 0;
}

static int change(uintptr_t addr, size_t size, int protect) {
  if ((addr | size) & (DIRTYTRACK_PAGE - 1) || addr < DIRTYTRACK_BASE || addr + size > DIRTYTRACK_END) {
    snprintf(g_error, sizeof g_error, "range %lx+%zx is not page aligned inside the tracked window", (unsigned long)addr, size);
    return -1;
  }
  if (mprotect((void*)addr, size, protect ? PROT_READ : PROT_READ | PROT_WRITE)) {
    snprintf(g_error, sizeof g_error, "mprotect(%lx, %zx): %s", (unsigned long)addr, size, strerror(errno));
    return -1;
  }
  memset(g_protected + (addr - DIRTYTRACK_BASE) / DIRTYTRACK_PAGE, protect, size / DIRTYTRACK_PAGE);
  return 0;
}
int dirtytrack_protect(uintptr_t addr, size_t size) { return change(addr, size, 1); }
int dirtytrack_release(uintptr_t addr, size_t size) { return change(addr, size, 0); }

void dirtytrack_prewrite(void* addr, size_t size) {
  if (!size) return;
  const uintptr_t first = (uintptr_t)addr & ~(DIRTYTRACK_PAGE - 1), last = ((uintptr_t)addr + size - 1) & ~(DIRTYTRACK_PAGE - 1);
  for (uintptr_t page = first; page <= last; page += DIRTYTRACK_PAGE)
    if (dirtytrack_is_protected(page)) first_write(page);
}
