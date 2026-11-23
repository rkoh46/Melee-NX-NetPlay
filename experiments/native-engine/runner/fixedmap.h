// Fixed-address memory for the native engine: the console's main memory at 0x80000000, the game
// image at 0x82800000 and the locked cache at 0xE0000000. One small interface with two back ends:
// fixedmap_horizon.c (svcMapProcessCodeMemory, NOT yet verified on hardware) and fixedmap_linux.c
// (mmap, used to test everything else on aarch64 Linux).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { FIXEDMAP_R = 1, FIXEDMAP_RW = 3, FIXEDMAP_RX = 5 };
#define FIXEDMAP_PAGE 0x1000u

// Zeroed, page-aligned staging pages. Fill them completely BEFORE fixedmap_commit: on Horizon the
// staging pages become inaccessible once they are mapped at their fixed address, and the fixed
// address is not readable or writable until fixedmap_protect has been called on it.
void* fixedmap_stage(size_t size);
// Makes the staged pages appear at `addr` (page aligned, range must be free). Returns 0 on success.
// After a failure the staging pages are released.
int fixedmap_commit(uintptr_t addr, void* staged, size_t size, const char* name);
// Sets the protection of a page-aligned part of a committed range. Returns 0 on success.
int fixedmap_protect(uintptr_t addr, size_t size, int prot);
// Unmaps one committed range (by its start address) or all of them. Safe to call twice.
void fixedmap_release(uintptr_t addr);
void fixedmap_release_all(void);
// The staging pages behind a committed address (Horizon: the heap pages that are mapped there; they
// are needed to unmap or remap part of a range). NULL when the address is not in a committed range
// or the back end has no such thing.
void* fixedmap_backing(uintptr_t addr);
// Text of the last failure, for the log.
const char* fixedmap_error(void);

// ---- the process layout, and starting over when it is in the way (Horizon) ----------------------
// Horizon places the heap region (8 GiB) and the alias region (64 GiB) of a 39-bit process at random
// addresses, once, when the process is created. About one launch in 70 puts one of them over the
// fixed ranges, and nothing can be mapped there for the life of that process (the kernel refuses the
// whole region, used or not; memory that already lies there cannot be made executable). The only
// cure is a new process. A program that is its own title (a forwarder that always loads this NRO)
// can ask the system to start the title again; under the Homebrew Menu a restart would start the
// menu or the overridden game instead, so it is only done when the arguments contain --relaunch.
//
// fixedmap_startup: call first in main(), before any thread is made. `ranges` is `count` pairs of
// (address, size). When this process was started by fixedmap_relaunch, *argc and *argv become the
// arguments of the first launch. Returns 0 when every range can be mapped (they are then kept away
// from libnx's own address choices), 1 when they cannot but fixedmap_relaunch can start over, -1
// when they cannot and it cannot (fixedmap_error says why). Always 0 on Linux.
// Arguments it reads (and leaves in place): --relaunch, and --simulate-bad-layout N, which puts a
// page of ours on the first range in the first N launches so the path can be tested.
int fixedmap_startup(int* argc, char*** argv, const uintptr_t* ranges, int count);
// One line for the log: where the regions are and what was decided.
const char* fixedmap_layout(void);
// How many times this run has been started over (0 on the first launch).
int fixedmap_relaunch_count(void);
// Starts the title again with the same arguments. Does not return when it works; -1 otherwise.
int fixedmap_relaunch(void);

#ifdef __cplusplus
}
#endif
