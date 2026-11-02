/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Copying to and from user memory. Every instruction that touches user memory is listed in the
 * __ex_table section with a recovery address: if it faults (and the page fault handler cannot map the
 * page), the exception handler resumes there instead of panicking, and the copy reports failure.
 *
 * With SMAP on, the kernel can't touch user memory even by accident: these are the one place that
 * means to. stac and clac are what say so,
 * on a processor that has them: a flag decides, as patching code would not be W^X.
 */

.macro USER_ACCESS fixup, instruction:vararg
9999: \instruction
    .pushsection __ex_table, "a"
    .balign 8
    .quad 9999b, \fixup
    .popsection
.endm

.macro USER_ACCESS_BEGIN
    cmpb $0, x86_64_has_smap(%rip)
    je 9998f
    stac
9998:
.endm

.macro USER_ACCESS_END
    cmpb $0, x86_64_has_smap(%rip)
    je 9997f
    clac
9997:
.endm

.text

/* u64 arch_copy_user(void* destination, void const* source, u64 size): 0, or 1 on a fault. */
.global arch_copy_user
.type arch_copy_user, @function
arch_copy_user:
    USER_ACCESS_BEGIN
    testq %rdx, %rdx
    jz 2f
    /* Eight bytes at a time while both are aligned, then bytes. */
    movq %rdi, %rax
    orq %rsi, %rax
    testb $7, %al
    jnz 1f
3:  cmpq $8, %rdx
    jb 1f
    USER_ACCESS 4f, movq (%rsi), %rax
    USER_ACCESS 4f, movq %rax, (%rdi)
    addq $8, %rsi
    addq $8, %rdi
    subq $8, %rdx
    jnz 3b
    jmp 2f
1:  USER_ACCESS 4f, movb (%rsi), %al
    USER_ACCESS 4f, movb %al, (%rdi)
    incq %rsi
    incq %rdi
    decq %rdx
    jnz 1b
2:  USER_ACCESS_END
    xorl %eax, %eax
    ret
4:  USER_ACCESS_END
    movl $1, %eax
    ret
.size arch_copy_user, . - arch_copy_user

/*
 * i64 arch_copy_user_string(char* destination, char const* source, u64 capacity): the length (without
 * the terminator) once a NUL was copied; -1 on a fault; -2 if no NUL appeared within `capacity` bytes.
 */
.global arch_copy_user_string
.type arch_copy_user_string, @function
arch_copy_user_string:
    USER_ACCESS_BEGIN
    xorl %ecx, %ecx
1:  cmpq %rdx, %rcx
    jae 3f
    USER_ACCESS 4f, movb (%rsi, %rcx), %al
    movb %al, (%rdi, %rcx)
    testb %al, %al
    jz 2f
    incq %rcx
    jmp 1b
2:  USER_ACCESS_END
    movq %rcx, %rax
    ret
3:  USER_ACCESS_END
    movq $-2, %rax
    ret
4:  USER_ACCESS_END
    movq $-1, %rax
    ret
.size arch_copy_user_string, . - arch_copy_user_string
