// Timing wrappers for the two interface tables between the game image and the host (hostprof.h).
// Every table entry is replaced by one of the thunks below; a call then goes thunk -> the original
// function -> mu_prof_return, which adds one call and the elapsed counter ticks to that entry's
// statistics. Nested calls (the host calling back into the game, the game calling the host again)
// are kept on a small shadow stack so each entry gets its own "exclusive" time.
// Register use follows the procedure call standard: x16/x17 on the way in (the linker may use them
// between any call and its target), x9-x17 on the way back (caller-saved, dead after a return).
// Arguments, x8 and results are not touched. One thread only.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef __SWITCH__
#define COUNTER cntpct_el0      /* what libnx's armGetSystemTick reads */
#else
#define COUNTER cntvct_el0
#endif
#define SLOTS 128

    .section .bss.mu_prof, "aw", %nobits
    .balign 16
    .global mu_prof_stats, mu_prof_orig, mu_prof_sp, mu_prof_stack
mu_prof_stats:  .space SLOTS * 32       // per slot: calls, exclusive ticks, inclusive ticks, unused
mu_prof_orig:   .space SLOTS * 8
mu_prof_stack:  .space 64 * 32          // per level: saved lr, slot, start tick, ticks spent in nested slots
mu_prof_sp:     .space 8                // next free level (set to mu_prof_stack by hostprof.cpp)

    .text
    .balign 4
    .global mu_prof_thunks
mu_prof_thunks:
    .set slot, 0
    .rept SLOTS
    mov     w16, #slot
    b       mu_prof_enter
    .set slot, slot + 1
    .endr

mu_prof_enter:
    adrp    x17, mu_prof_sp
    ldr     x17, [x17, :lo12:mu_prof_sp]
    stp     x30, x16, [x17, #0]
    mrs     x30, COUNTER
    stp     x30, xzr, [x17, #16]
    add     x17, x17, #32
    adrp    x30, mu_prof_sp
    str     x17, [x30, :lo12:mu_prof_sp]
    adrp    x17, mu_prof_orig
    add     x17, x17, :lo12:mu_prof_orig
    ldr     x17, [x17, x16, lsl #3]
    adr     x30, mu_prof_return
    br      x17

mu_prof_return:
    adrp    x9, mu_prof_sp
    ldr     x10, [x9, :lo12:mu_prof_sp]
    sub     x10, x10, #32
    str     x10, [x9, :lo12:mu_prof_sp]
    ldp     x30, x11, [x10, #0]         // return address, slot
    ldp     x12, x13, [x10, #16]        // start, nested
    mrs     x14, COUNTER
    sub     x14, x14, x12               // inclusive
    adrp    x15, mu_prof_stats
    add     x15, x15, :lo12:mu_prof_stats
    add     x15, x15, x11, lsl #5
    ldp     x16, x17, [x15, #0]
    add     x16, x16, #1
    sub     x12, x14, x13               // exclusive
    add     x17, x17, x12
    stp     x16, x17, [x15, #0]
    ldr     x16, [x15, #16]
    add     x16, x16, x14
    str     x16, [x15, #16]
    adrp    x16, mu_prof_stack
    add     x16, x16, :lo12:mu_prof_stack
    cmp     x10, x16
    b.ls    1f
    ldr     x17, [x10, #-8]             // the caller's "nested" field
    add     x17, x17, x14
    str     x17, [x10, #-8]
1:  ret
#ifndef __SWITCH__
    .section .note.GNU-stack,"",%progbits
#endif
