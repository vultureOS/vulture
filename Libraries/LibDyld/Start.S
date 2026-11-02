/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Where the kernel starts a dynamically linked program: here, with the
// program's initial stack (argc, argv, environ, the auxiliary vector).
// dyld_main loads and links everything and returns the program's entry
// point, which then starts with that same stack, as if nothing came first.

#if defined(__x86_64__)

.text
.global _dyld_start
.type _dyld_start, @function
_dyld_start:
    xorl %ebp, %ebp
    movq %rsp, %r12
    andq $-16, %rsp
    // Nothing that takes an absolute address works until this has run.
    leaq __ehdr_start(%rip), %rdi
    leaq _DYNAMIC(%rip), %rsi
    call dyld_relocate_self
    movq %r12, %rdi
    call dyld_main
    movq %r12, %rsp
    xorl %edx, %edx
    jmpq *%rax
.size _dyld_start, . - _dyld_start

#else

.text
.global _dyld_start
.type _dyld_start, %function
_dyld_start:
    mov x29, xzr
    mov x30, xzr
    // Nothing that takes an absolute address works until this has run.
    adrp x0, __ehdr_start
    add x0, x0, :lo12:__ehdr_start
    adrp x1, _DYNAMIC
    add x1, x1, :lo12:_DYNAMIC
    bl dyld_relocate_self
    mov x0, sp
    bl dyld_main
    mov x16, x0
    mov x0, xzr
    mov x29, xzr
    mov x30, xzr
    br x16
.size _dyld_start, . - _dyld_start

// What a TLS descriptor's first word points at: returns, in x0, the offset
// from the thread pointer held in its second word (x0 points at the
// descriptor). Its callers count on every other register being kept.
.global dyld_tlsdesc_static
.type dyld_tlsdesc_static, %function
dyld_tlsdesc_static:
    bti c
    ldr x0, [x0, #8]
    ret
.size dyld_tlsdesc_static, . - dyld_tlsdesc_static

#endif
