/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__x86_64__)

/* jmp_buf: rbx, rbp, r12-r15, rsp, the return address: 8 words of the 22. */

.text
.global setjmp
.type setjmp, @function
setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, @function
longjmp:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    /* setjmp appears to return the value, or 1 if it is 0. */
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:  jmpq *56(%rdi)
.size longjmp, . - longjmp

#else

/* jmp_buf: x19-x30 (12), sp, d8-d15 (8), padding: 22 words. */

.text
.global setjmp
.type setjmp, %function
setjmp:
    bti c
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x1, sp
    str x1, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov x0, #0
    ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, %function
longjmp:
    bti c
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    /* setjmp appears to return the value, or 1 if it is 0. */
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret
.size longjmp, . - longjmp

#endif
