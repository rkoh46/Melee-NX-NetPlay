// The program's user exception entry, replacing libnx's weak __libnx_exception_entry. The crt0
// branches here when the kernel reports an exception: x0 = exception type, x1 = the
// ThreadExceptionFrameA64 in thread-local storage holding the interrupted code's x0-x8, lr, sp, pc
// and pstate, sp = a tiny kernel-provided stack that is not used here.
//
// Resuming a faulting instruction exactly (checked against the kernel source, Atmosphere's
// mesosphere): svcReturnFromException(0) restores x0-x8, lr, sp, pc and pstate from that frame, and
// every other general register from the kernel's saved context of this thread. The SVC entry saves
// x9-x11 and x18-x29 into that context, but NOT x12-x17: those come back as they were at the last
// full save, which is the fault itself unless an interrupt arrived while the handler ran, in which
// case they would be whatever the handler had in them. So the retry takes two trips:
//
//   trip 1 (the real fault)   save everything the C handler may clobber, call it (it makes the page
//                             writable and points the frame's pc at mu_fault_resume_stub), return.
//   mu_fault_resume_stub      back in normal context: restore x9-x18, x29, the vector registers,
//                             fpcr and fpsr, then fault on purpose by reading address 0x10. That
//                             fault makes the kernel save the full, now correct, register set.
//   trip 2 (the stub's fault) using x2-x8 only: put the original frame back and return. From the
//                             stub's fault to here x12-x17 hold their original values, so an
//                             interrupt at any point saves the right ones.
//
// x19-x28 are callee-saved for the C handler and untouched elsewhere.
// SPDX-License-Identifier: GPL-3.0-or-later
    .section .bss.mu_fault_state, "aw", %nobits
    .balign 16
mu_fault_stack:
    .space 0x8000
mu_fault_stack_end:
mu_fault_save:                      // x9-x18 (0x00), x29 (0x50), fpcr, fpsr (0x58, 0x60), q0-q31 (0x70)
    .space 0x270

    .section .text.__libnx_exception_entry, "ax", %progbits
    .global __libnx_exception_entry
    .type __libnx_exception_entry, %function
    .balign 4
__libnx_exception_entry:
    cbz     x1, 9f
    adrp    x2, mu_fault_resume_pending
    add     x2, x2, :lo12:mu_fault_resume_pending
    ldr     w3, [x2]
    cbz     w3, 2f

    // Trip 2: only x2-x8 from here to the svc.
    ldr     x4, [x1, #0x70]         // far
    cmp     x4, #0x10
    b.ne    9f
    str     wzr, [x2]
    adrp    x5, mu_fault_frame
    add     x5, x5, :lo12:mu_fault_frame
    ldp     x6, x7, [x5, #0x00]
    stp     x6, x7, [x1, #0x00]
    ldp     x6, x7, [x5, #0x10]
    stp     x6, x7, [x1, #0x10]
    ldp     x6, x7, [x5, #0x20]
    stp     x6, x7, [x1, #0x20]
    ldp     x6, x7, [x5, #0x30]
    stp     x6, x7, [x1, #0x30]
    ldp     x6, x7, [x5, #0x40]     // x8, lr
    stp     x6, x7, [x1, #0x40]
    ldp     x6, x7, [x5, #0x50]     // sp, pc
    stp     x6, x7, [x1, #0x50]
    ldr     w6, [x5, #0x60]         // pstate
    str     w6, [x1, #0x60]
    mov     w0, wzr
    svc     0x28
    b       .

2:  // Trip 1.
    adrp    x2, mu_fault_stack_end
    add     x2, x2, :lo12:mu_fault_stack_end
    mov     sp, x2
    adrp    x2, mu_fault_save
    add     x2, x2, :lo12:mu_fault_save
    stp     x9, x10, [x2, #0x00]
    stp     x11, x12, [x2, #0x10]
    stp     x13, x14, [x2, #0x20]
    stp     x15, x16, [x2, #0x30]
    stp     x17, x18, [x2, #0x40]
    str     x29, [x2, #0x50]
    mrs     x3, fpcr
    mrs     x4, fpsr
    stp     x3, x4, [x2, #0x58]
    add     x2, x2, #0x70
    stp     q0, q1, [x2, #0x000]
    stp     q2, q3, [x2, #0x020]
    stp     q4, q5, [x2, #0x040]
    stp     q6, q7, [x2, #0x060]
    stp     q8, q9, [x2, #0x080]
    stp     q10, q11, [x2, #0x0a0]
    stp     q12, q13, [x2, #0x0c0]
    stp     q14, q15, [x2, #0x0e0]
    stp     q16, q17, [x2, #0x100]
    stp     q18, q19, [x2, #0x120]
    stp     q20, q21, [x2, #0x140]
    stp     q22, q23, [x2, #0x160]
    stp     q24, q25, [x2, #0x180]
    stp     q26, q27, [x2, #0x1a0]
    stp     q28, q29, [x2, #0x1c0]
    stp     q30, q31, [x2, #0x1e0]
    bl      mu_fault_handler
    cbnz    w0, 9f
    mov     w0, wzr
    svc     0x28                    // continues in mu_fault_resume_stub
    b       .

9:  mov     w0, #0xf801             // unhandled: the kernel ends the process (crash report)
    svc     0x28
    b       .
    .size __libnx_exception_entry, . - __libnx_exception_entry

    .section .text.mu_fault_resume_stub, "ax", %progbits
    .global mu_fault_resume_stub
    .type mu_fault_resume_stub, %function
    .balign 4
mu_fault_resume_stub:
    adrp    x0, mu_fault_save
    add     x0, x0, :lo12:mu_fault_save
    ldp     x1, x2, [x0, #0x58]
    msr     fpcr, x1
    msr     fpsr, x2
    add     x1, x0, #0x70
    ldp     q0, q1, [x1, #0x000]
    ldp     q2, q3, [x1, #0x020]
    ldp     q4, q5, [x1, #0x040]
    ldp     q6, q7, [x1, #0x060]
    ldp     q8, q9, [x1, #0x080]
    ldp     q10, q11, [x1, #0x0a0]
    ldp     q12, q13, [x1, #0x0c0]
    ldp     q14, q15, [x1, #0x0e0]
    ldp     q16, q17, [x1, #0x100]
    ldp     q18, q19, [x1, #0x120]
    ldp     q20, q21, [x1, #0x140]
    ldp     q22, q23, [x1, #0x160]
    ldp     q24, q25, [x1, #0x180]
    ldp     q26, q27, [x1, #0x1a0]
    ldp     q28, q29, [x1, #0x1c0]
    ldp     q30, q31, [x1, #0x1e0]
    ldp     x9, x10, [x0, #0x00]
    ldp     x11, x12, [x0, #0x10]
    ldp     x13, x14, [x0, #0x20]
    ldp     x15, x16, [x0, #0x30]
    ldp     x17, x18, [x0, #0x40]
    ldr     x29, [x0, #0x50]
    mov     x1, #0x10
    ldr     x1, [x1]                // deliberate data abort, far = 0x10 -> trip 2
    b       .
    .size mu_fault_resume_stub, . - mu_fault_resume_stub
