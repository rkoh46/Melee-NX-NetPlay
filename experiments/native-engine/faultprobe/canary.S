// u64 mu_canary_store(u64* address, u64 seed)
// Fills every register a fault handler could damage with values derived from `seed`, performs one
// store to `address` (which faults when the page is read-only), then checks them all.
// Returns a mask of what changed: bits 2..28 = x2..x28, bit 29 = NZCV flags, bit 30 = low half of
// some vector register, bit 31 = high half of some vector register, bit 32 = fpsr, bit 33 = fpcr.
// SPDX-License-Identifier: GPL-3.0-or-later
    .text
    .global mu_canary_store
    .type mu_canary_store, %function
    .balign 4
mu_canary_store:
    sub     sp, sp, #0xb0
    stp     x19, x20, [sp, #0x00]
    stp     x21, x22, [sp, #0x10]
    stp     x23, x24, [sp, #0x20]
    stp     x25, x26, [sp, #0x30]
    stp     x27, x28, [sp, #0x40]
    stp     x29, x30, [sp, #0x50]
    stp     d8, d9, [sp, #0x60]
    stp     d10, d11, [sp, #0x70]
    stp     d12, d13, [sp, #0x80]
    stp     d14, d15, [sp, #0x90]
    mrs     x29, fpsr
    str     x29, [sp, #0xa0]

    // Vector registers: both halves = seed + 100 + n.
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    add     x30, x1, #(100 + \n)
    dup     v\n\().2d, x30
    .endr
    // fpsr: set the cumulative inexact and overflow flags (bits 4 and 2), so a handler that
    // clears or adds flags is noticed.
    mov     x30, #0x14
    msr     fpsr, x30
    // General registers x2..x28 = seed + n.
    .irp n, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28
    add     x\n, x1, #\n
    .endr
    mov     x29, xzr
    cmp     x1, x1                  // Z = 1, C = 1, N = 0, V = 0
    str     x1, [x0]                // <- the store under test
    mrs     x30, nzcv
    eor     x30, x30, #0x60000000
    cbz     x30, 1f
    orr     x29, x29, #(1 << 29)
1:
    .irp n, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28
    add     x30, x1, #\n
    cmp     x\n, x30
    b.eq    2f
    orr     x29, x29, #(1 << \n)
2:
    .endr
    // x2 and x3 are checked: use them as scratch for the vector halves.
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    add     x30, x1, #(100 + \n)
    mov     x2, v\n\().d[0]
    mov     x3, v\n\().d[1]
    cmp     x2, x30
    b.eq    3f
    orr     x29, x29, #(1 << 30)
3:  cmp     x3, x30
    b.eq    4f
    orr     x29, x29, #(1 << 31)
4:
    .endr
    mrs     x30, fpsr
    cmp     x30, #0x14
    b.eq    5f
    orr     x29, x29, #(1 << 32)
5:  mrs     x30, fpcr
    cbz     x30, 6f
    orr     x29, x29, #(1 << 33)
6:
    mov     x0, x29
    ldr     x29, [sp, #0xa0]
    msr     fpsr, x29
    ldp     x19, x20, [sp, #0x00]
    ldp     x21, x22, [sp, #0x10]
    ldp     x23, x24, [sp, #0x20]
    ldp     x25, x26, [sp, #0x30]
    ldp     x27, x28, [sp, #0x40]
    ldp     x29, x30, [sp, #0x50]
    ldp     d8, d9, [sp, #0x60]
    ldp     d10, d11, [sp, #0x70]
    ldp     d12, d13, [sp, #0x80]
    ldp     d14, d15, [sp, #0x90]
    add     sp, sp, #0xb0
    ret
    .size mu_canary_store, . - mu_canary_store
