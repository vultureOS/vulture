/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Kernel entry from VultureBoot. On arrival: long mode, the loader's page tables (the kernel at its
 * link address, physical memory in the direct map), interrupts masked, rdi = virtual address of the
 * BootInfo.
 */

.section .text.boot, "ax"

.global _start
.type _start, @function
_start:
    leaq boot_stack_top(%rip), %rsp
    /* A zero frame pointer ends backtraces here. */
    xorl %ebp, %ebp
    /* Take over exceptions, and everything else about this processor. Keeps the BootInfo. */
    pushq %rdi
    subq $8, %rsp
    call x86_64_initialize_boot_processor
    addq $8, %rsp
    popq %rdi
    call kernel_main
1:  hlt
    jmp 1b
.size _start, . - _start

/*
 * Secondary processors start here, in real mode, at the start of a page of low memory that this code
 * has been copied to (SMP.cpp), with what they need in the block after it. Every one of the modes
 * between that and the kernel has its own idea of addresses, so ebx keeps where this page is.
 */
#define OFFSET(label) ((label) - x86_64_trampoline_start)

.section .rodata.trampoline, "a"
.balign 16
.code16
.global x86_64_trampoline_start
x86_64_trampoline_start:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    movzwl %ax, %ebx
    shll $4, %ebx
    /* Where its table is, and where to jump to, depend on where it is. */
    leal OFFSET(trampoline_gdt)(%ebx), %eax
    movl %eax, OFFSET(trampoline_gdt_pointer) + 2
    leal OFFSET(protected_mode)(%ebx), %eax
    movl %eax, OFFSET(far_to_protected_mode)
    leal OFFSET(long_mode)(%ebx), %eax
    movl %eax, OFFSET(far_to_long_mode)
    lgdtl OFFSET(trampoline_gdt_pointer)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl *OFFSET(far_to_protected_mode)

.code32
protected_mode:
    movl $0x10, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %ss
    /* PAE, for long mode; then the tables that map this page where it is. */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4
    movl OFFSET(x86_64_trampoline_parameters)(%ebx), %eax
    movl %eax, %cr3
    /* EFER: long mode, no-execute pages. */
    movl $0xc0000080, %ecx
    rdmsr
    orl $((1 << 8) | (1 << 11)), %eax
    wrmsr
    /* Paging, and write protection in the kernel too. */
    movl %cr0, %eax
    orl $((1 << 31) | (1 << 16)), %eax
    movl %eax, %cr0
    ljmpl *OFFSET(far_to_long_mode)(%ebx)

.code64
long_mode:
    movl %ebx, %ebx
    /* Tables that have the kernel too, which can be anywhere in memory. On its stack, and on to it. */
    movq OFFSET(x86_64_trampoline_parameters) + 8(%rbx), %rax
    movq OFFSET(x86_64_trampoline_parameters) + 16(%rbx), %rsp
    movq OFFSET(x86_64_trampoline_parameters) + 24(%rbx), %rcx
    leaq OFFSET(x86_64_trampoline_parameters)(%rbx), %rdi
    movq %rax, %cr3
    xorl %ebp, %ebp
    jmpq *%rcx

.balign 16
trampoline_gdt:
    .quad 0
    .quad 0x00cf9a000000ffff /* 32-bit code */
    .quad 0x00cf92000000ffff /* data */
    .quad 0x00af9a000000ffff /* 64-bit code */
trampoline_gdt_pointer:
    .word trampoline_gdt_pointer - trampoline_gdt - 1
    .long 0
.balign 4
far_to_protected_mode:
    .long 0
    .word 0x08
.balign 4
far_to_long_mode:
    .long 0
    .word 0x18

/* Read by the above, and written by SMP.cpp: see StartupParameters. */
.balign 16
.global x86_64_trampoline_parameters
x86_64_trampoline_parameters:
    .space 64
.global x86_64_trampoline_end
x86_64_trampoline_end:

.code64
.section .bss.boot_stack, "aw", @nobits
.balign 16
.global boot_stack_bottom
boot_stack_bottom:
    .space 64 * 1024
.global boot_stack_top
boot_stack_top:
