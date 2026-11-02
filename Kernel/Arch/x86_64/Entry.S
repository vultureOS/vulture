/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Every way into and out of the kernel: exceptions and interrupts through the interrupt descriptor
 * table, system calls through the syscall instruction, and thread switches. See TrapFrame.h for the
 * frame they all build, and ProcessorArea in CPU.cpp for what %gs points at in the kernel:
 */

#define AREA_SCRATCH 8
#define AREA_KERNEL_STACK 16
#define AREA_TSS 24

#define KERNEL_CODE 0x08
#define USER_DATA 0x1b
#define USER_CODE 0x23
#define VECTOR_SYSCALL 0x100

#define FRAME_CR2 120
#define FRAME_RECORD 128
#define FRAME_RBP 80
#define FRAME_RIP 168
#define FRAME_CS 176
#define FRAME_SIZE 208

/* Exceptions that push an error code of their own: 8, 10-14, 17, 21, 29, 30. */
.macro STUB vector
    .balign 16
    .if (\vector == 8) || (\vector >= 10 && \vector <= 14) || (\vector == 17) || (\vector == 21) || (\vector == 29) || (\vector == 30)
    .else
    pushq $0
    .endif
    pushq $\vector
    jmp interrupt_entry
.endm

.section .text.vectors, "ax"
.balign 16
.global x86_64_interrupt_stubs
x86_64_interrupt_stubs:
.macro STUBS base
    STUB (\base + 0)
    STUB (\base + 1)
    STUB (\base + 2)
    STUB (\base + 3)
    STUB (\base + 4)
    STUB (\base + 5)
    STUB (\base + 6)
    STUB (\base + 7)
    STUB (\base + 8)
    STUB (\base + 9)
    STUB (\base + 10)
    STUB (\base + 11)
    STUB (\base + 12)
    STUB (\base + 13)
    STUB (\base + 14)
    STUB (\base + 15)
.endm
    STUBS 0x00
    STUBS 0x10
    STUBS 0x20
    STUBS 0x30
    STUBS 0x40
    STUBS 0x50
    STUBS 0x60
    STUBS 0x70
    STUBS 0x80
    STUBS 0x90
    STUBS 0xa0
    STUBS 0xb0
    STUBS 0xc0
    STUBS 0xd0
    STUBS 0xe0
    STUBS 0xf0

.text

/* What both entries share, from the error code on down. */
.macro SAVE_REGISTERS
    subq $32, %rsp
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.type interrupt_entry, @function
interrupt_entry:
    cld
    SAVE_REGISTERS
    movq %cr2, %rax
    movq %rax, FRAME_CR2(%rsp)
    /*
     * A frame record for the interrupted code, so backtraces taken in a handler continue into
     * whatever the exception interrupted. From user mode the chain ends here: user frames are not
     * the kernel's to follow.
     */
    xorl %eax, %eax
    testb $3, FRAME_CS(%rsp)
    jz 1f
    swapgs
    jmp 2f
1:  movq %rbp, %rax
2:  movq %rax, FRAME_RECORD(%rsp)
    movq FRAME_RIP(%rsp), %rax
    movq %rax, FRAME_RECORD + 8(%rsp)
    leaq FRAME_RECORD(%rsp), %rbp
    movq %rsp, %rdi
    call x86_64_handle_exception

/* Restores the trap frame at rsp, and returns from the exception. */
.global x86_64_exception_return
x86_64_exception_return:
    testb $3, FRAME_CS(%rsp)
    jz 1f
    /*
     * Into user mode. The next time it comes back, it starts again from where this frame ends: this
     * thread's kernel stack, empty. A thread moving between processors takes this with it.
     */
    cli
    leaq FRAME_SIZE(%rsp), %rax
    movq %rax, %gs:AREA_KERNEL_STACK
    movq %gs:AREA_TSS, %rcx
    movq %rax, 4(%rcx)
    swapgs
1:  popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $48, %rsp
    iretq
.size interrupt_entry, . - interrupt_entry

/*
 * The syscall instruction: the return address is in rcx, the flags in r11, and interrupts are off (the
 * flags mask has seen to that). There's no stack yet. A frame just like an interrupt's, so that
 * everything from here on, including going back, is the same.
 */
.global x86_64_syscall_entry
.type x86_64_syscall_entry, @function
x86_64_syscall_entry:
    swapgs
    movq %rsp, %gs:AREA_SCRATCH
    movq %gs:AREA_KERNEL_STACK, %rsp
    pushq $USER_DATA
    pushq %gs:AREA_SCRATCH
    pushq %r11
    pushq $USER_CODE
    pushq %rcx
    pushq $0
    pushq $VECTOR_SYSCALL
    cld
    SAVE_REGISTERS
    movq $0, FRAME_CR2(%rsp)
    movq $0, FRAME_RECORD(%rsp)
    movq %rcx, FRAME_RECORD + 8(%rsp)
    leaq FRAME_RECORD(%rsp), %rbp
    movq %rsp, %rdi
    call x86_64_handle_exception
    jmp x86_64_exception_return
.size x86_64_syscall_entry, . - x86_64_syscall_entry

/* void x86_64_return_to_user(TrapFrame* frame): enters user mode through a trap frame. */
.global x86_64_return_to_user
.type x86_64_return_to_user, @function
x86_64_return_to_user:
    cli
    movq %rdi, %rsp
    jmp x86_64_exception_return
.size x86_64_return_to_user, . - x86_64_return_to_user

/*
 * The processor that gets this has been told to stop: a panic is in progress somewhere. It comes as
 * a non-maskable interrupt, on a stack of its own, so as to stop a processor spinning with interrupts
 * masked; so this does nothing that needs anything to be in any particular state.
 */
.global x86_64_stop_processor
.type x86_64_stop_processor, @function
x86_64_stop_processor:
    cli
1:  hlt
    jmp 1b
.size x86_64_stop_processor, . - x86_64_stop_processor

/*
 * x86_64_switch_context(rdi = from, rsi = to) saves what the calling convention says survives a call
 * plus the SSE state and the user's thread pointer, loads the same from `to`, and returns into it.
 */
.global x86_64_switch_context
.type x86_64_switch_context, @function
x86_64_switch_context:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    movq %rsp, 48(%rdi)
    fxsave64 64(%rdi)
    /* The thread pointer: MSR 0xc0000100, FS base. */
    movl $0xc0000100, %ecx
    rdmsr
    shlq $32, %rdx
    orq %rdx, %rax
    movq %rax, 56(%rdi)

    movq 56(%rsi), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    wrmsr
    fxrstor64 64(%rsi)
    movq 0(%rsi), %rbx
    movq 8(%rsi), %rbp
    movq 16(%rsi), %r12
    movq 24(%rsi), %r13
    movq 32(%rsi), %r14
    movq 40(%rsi), %r15
    movq 48(%rsi), %rsp
    ret
.size x86_64_switch_context, . - x86_64_switch_context

/*
 * A new thread's first "return" from x86_64_switch_context lands here, with its entry point in rbx
 * and argument in r12.
 */
.global x86_64_thread_trampoline
.type x86_64_thread_trampoline, @function
x86_64_thread_trampoline:
    movq %rbx, %rdi
    movq %r12, %rsi
    xorl %ebp, %ebp
    call kernel_thread_start
    ud2
.size x86_64_thread_trampoline, . - x86_64_thread_trampoline

/* The SSE state, for signal frames and fork. rdi = 512 bytes, aligned to 16. */
.global x86_64_save_fp_state
.type x86_64_save_fp_state, @function
x86_64_save_fp_state:
    fxsave64 (%rdi)
    ret
.size x86_64_save_fp_state, . - x86_64_save_fp_state

.global x86_64_load_fp_state
.type x86_64_load_fp_state, @function
x86_64_load_fp_state:
    fxrstor64 (%rdi)
    ret
.size x86_64_load_fp_state, . - x86_64_load_fp_state
