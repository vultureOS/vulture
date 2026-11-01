/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Thread switching; the layout is ThreadContext (ThreadContext.h).
 *
 * aarch64_switch_context(x0 = from, x1 = to) saves what the calling
 * convention says survives a call (x19-x30, sp) plus the FP/SIMD state and
 * the user's thread pointer, loads the same from `to`, and returns into it.
 */

.text

.global aarch64_switch_context
.type aarch64_switch_context, %function
aarch64_switch_context:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    mrs x10, fpcr
    stp x9, x10, [x0, #96]
    mrs x9, fpsr
    mrs x10, tpidr_el0
    stp x9, x10, [x0, #112]

    add x9, x0, #128
    stp q0, q1, [x9, #0]
    stp q2, q3, [x9, #32]
    stp q4, q5, [x9, #64]
    stp q6, q7, [x9, #96]
    stp q8, q9, [x9, #128]
    stp q10, q11, [x9, #160]
    stp q12, q13, [x9, #192]
    stp q14, q15, [x9, #224]
    stp q16, q17, [x9, #256]
    stp q18, q19, [x9, #288]
    stp q20, q21, [x9, #320]
    stp q22, q23, [x9, #352]
    stp q24, q25, [x9, #384]
    stp q26, q27, [x9, #416]
    stp q28, q29, [x9, #448]
    stp q30, q31, [x9, #480]

    add x9, x1, #128
    ldp q0, q1, [x9, #0]
    ldp q2, q3, [x9, #32]
    ldp q4, q5, [x9, #64]
    ldp q6, q7, [x9, #96]
    ldp q8, q9, [x9, #128]
    ldp q10, q11, [x9, #160]
    ldp q12, q13, [x9, #192]
    ldp q14, q15, [x9, #224]
    ldp q16, q17, [x9, #256]
    ldp q18, q19, [x9, #288]
    ldp q20, q21, [x9, #320]
    ldp q22, q23, [x9, #352]
    ldp q24, q25, [x9, #384]
    ldp q26, q27, [x9, #416]
    ldp q28, q29, [x9, #448]
    ldp q30, q31, [x9, #480]

    ldp x9, x10, [x1, #96]
    mov sp, x9
    msr fpcr, x10
    ldp x9, x10, [x1, #112]
    msr fpsr, x9
    msr tpidr_el0, x10
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ret
.size aarch64_switch_context, . - aarch64_switch_context

/*
 * A new thread's first "return" from aarch64_switch_context lands here,
 * with its entry point in x19 and argument in x20.
 */
.global aarch64_thread_trampoline
.type aarch64_thread_trampoline, %function
aarch64_thread_trampoline:
    mov x0, x19
    mov x1, x20
    bl kernel_thread_start
    brk #0
.size aarch64_thread_trampoline, . - aarch64_thread_trampoline

/*
 * The FP/SIMD state, for signal frames and fork. x0 = 66 words: q0-q31,
 * FPCR, FPSR. Kernel code never touches these registers, so they hold the
 * current thread's user state whenever it is in the kernel.
 */
.global aarch64_save_fp_state
.type aarch64_save_fp_state, %function
aarch64_save_fp_state:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x9, fpcr
    mrs x10, fpsr
    str x9, [x0, #512]
    str x10, [x0, #520]
    ret
.size aarch64_save_fp_state, . - aarch64_save_fp_state

.global aarch64_load_fp_state
.type aarch64_load_fp_state, %function
aarch64_load_fp_state:
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ldr x9, [x0, #512]
    ldr x10, [x0, #520]
    msr fpcr, x9
    msr fpsr, x10
    ret
.size aarch64_load_fp_state, . - aarch64_load_fp_state
