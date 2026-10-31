/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The exception vector table: 16 entries of 128 bytes, for the four
 * exception types (synchronous, IRQ, FIQ, SError) taken from the four
 * sources (current EL with SP0, current EL with SPx, lower EL in AArch64,
 * lower EL in AArch32). Every entry saves a TrapFrame (see TrapFrame.h) and
 * calls handle_exception(frame, entry index).
 */

#define TRAP_FRAME_SIZE 320

.macro VECTOR index
    .balign 0x80
    sub sp, sp, #TRAP_FRAME_SIZE
    stp x0, x1, [sp, #0]
    mov x0, #\index
    b exception_entry
.endm

.section .text.vectors, "ax"
.balign 0x800
.global exception_vector_table
exception_vector_table:
    VECTOR 0
    VECTOR 1
    VECTOR 2
    VECTOR 3
    VECTOR 4
    VECTOR 5
    VECTOR 6
    VECTOR 7
    VECTOR 8
    VECTOR 9
    VECTOR 10
    VECTOR 11
    VECTOR 12
    VECTOR 13
    VECTOR 14
    VECTOR 15

.type exception_entry, %function
exception_entry:
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    add x1, sp, #TRAP_FRAME_SIZE
    str x1, [sp, #248]
    mrs x1, elr_el1
    str x1, [sp, #256]
    mrs x1, spsr_el1
    str x1, [sp, #264]
    mrs x1, esr_el1
    str x1, [sp, #272]
    mrs x1, far_el1
    str x1, [sp, #280]
    mrs x1, sp_el0
    str x1, [sp, #304]

    /*
     * A frame record for the interrupted code, so backtraces taken in a
     * handler continue into whatever the exception interrupted. From user
     * mode the chain ends here: user frames are not the kernel's to follow.
     */
    ldr x1, [sp, #256]
    ldr x2, [sp, #264]
    tst x2, #0xf
    csel x3, x29, xzr, ne
    stp x3, x1, [sp, #288]
    add x29, sp, #288

    mov x1, x0
    mov x0, sp
    bl handle_exception

/* Restores the trap frame at sp and returns from the exception. */
.global exception_return
exception_return:
    ldr x1, [sp, #304]
    msr sp_el0, x1
    ldr x1, [sp, #256]
    msr elr_el1, x1
    ldr x1, [sp, #264]
    msr spsr_el1, x1
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #TRAP_FRAME_SIZE
    eret
.size exception_entry, . - exception_entry

/*
 * void aarch64_return_to_user(TrapFrame* frame): enters user mode through
 * a trap frame at the top of the current kernel stack, as if returning
 * from an exception (for new processes and forked children).
 */
.global aarch64_return_to_user
.type aarch64_return_to_user, %function
aarch64_return_to_user:
    msr daifset, #0xf
    mov sp, x0
    b exception_return
.size aarch64_return_to_user, . - aarch64_return_to_user
