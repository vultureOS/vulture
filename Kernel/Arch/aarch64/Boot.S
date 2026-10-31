/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Kernel entry from VultureBoot. On arrival: EL1, MMU on with the loader's
 * tables (the kernel at its link address, physical memory in the direct
 * map), interrupts masked, x0 = virtual address of the BootInfo.
 */

.section .text.boot, "ax"

.global _start
.type _start, %function
_start:
    /* Take over exceptions first, so even a fault in early setup is reported. */
    adrp x1, exception_vector_table
    add x1, x1, :lo12:exception_vector_table
    msr vbar_el1, x1
    isb

    adrp x1, boot_stack_top
    add x1, x1, :lo12:boot_stack_top
    mov sp, x1

    /* A zero frame pointer ends backtraces here. */
    mov x29, xzr
    mov x30, xzr
    bl kernel_main

1:  wfi
    b 1b
.size _start, . - _start

.section .bss.boot_stack, "aw", %nobits
.balign 16
.global boot_stack_bottom
boot_stack_bottom:
    .space 64 * 1024
.global boot_stack_top
boot_stack_top:
