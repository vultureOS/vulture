/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Where every program starts. The kernel leaves sp pointing at argc,
 * followed by argv[], a null, envp[], a null and the auxiliary vector.
 */

.text
.global _start
#if defined(__x86_64__)
.type _start, @function
_start:
    xorl %ebp, %ebp
    movq %rsp, %rdi
    andq $-16, %rsp
    call __libc_start
    ud2
#else
.type _start, %function
_start:
    bti c
    mov x29, xzr
    mov x30, xzr
    mov x0, sp
    bl __libc_start
    brk #0
#endif
.size _start, . - _start

// Each program (and each library) has its own: C++ destructors registered
// with __cxa_atexit name the object they belong to by it.
.data
.balign 8
.global __dso_handle
.hidden __dso_handle
__dso_handle:
    .quad __dso_handle
