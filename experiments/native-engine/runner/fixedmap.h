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

#ifdef __cplusplus
}
#endif
