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

    /* Run on SP_EL1, and start with no per-CPU pointer (see Processor). */
    msr spsel, #1
    msr tpidr_el1, xzr
    adrp x1, boot_stack_top
    add x1, x1, :lo12:boot_stack_top
    mov sp, x1

    /* A zero frame pointer ends backtraces here. */
    mov x29, xzr
    mov x30, xzr
    /*
     * Pointer authentication goes on before anything is signed: nothing signed before it could
     * be checked after. Keeps x0, the BootInfo, which this function only reads.
     */
    mov x19, x0
    bl aarch64_enable_pointer_authentication
    mov x0, x19
    bl kernel_main

1:  wfi
    b 1b
.size _start, . - _start

/*
 * Secondary processors start here, from PSCI CPU_ON: EL1, MMU and caches
 * off, x0 = physical address of their StartupParameters (see SMP.cpp).
 * Everything is loaded into registers before the MMU goes on, because
 * memory reads now bypass the cache.
 */
.section .text, "ax"
.global secondary_entry
.type secondary_entry, %function
secondary_entry:
    msr daifset, #0xf
    ldp x1, x2, [x0, #0]    /* mair, tcr */
    ldp x3, x4, [x0, #16]   /* ttbr0 (identity map), ttbr1 (kernel) */
    ldp x5, x6, [x0, #32]   /* sctlr, vbar */
    ldp x7, x8, [x0, #48]   /* stack_top, entry */
    ldr x9, [x0, #64]       /* argument */
    ldp x11, x12, [x0, #72] /* the kernel's pointer authentication key */
    ldr x13, [x0, #88]      /* whether there is such a thing */
    msr mair_el1, x1
    msr tcr_el1, x2
    msr ttbr0_el1, x3
    msr ttbr1_el1, x4
    isb
    tlbi vmalle1
    dsb nsh
    isb
    /* The registers only exist if the feature does. */
    cbz x13, 4f
    msr S3_0_C2_C1_2, x11   /* APIBKeyLo_EL1 */
    msr S3_0_C2_C1_3, x12   /* APIBKeyHi_EL1 */
4:
    msr sctlr_el1, x5
    isb
    /* Translation is on; continue at the kernel's own address. */
    ldr x10, =2f
    br x10
2:
    bti j
    msr vbar_el1, x6
    msr spsel, #1
    mov sp, x7
    msr tpidr_el1, xzr
    /* Drop the identity map: stop translating lower-half addresses. */
    mrs x10, tcr_el1
    orr x10, x10, #(1 << 7)
    msr tcr_el1, x10
    isb
    tlbi vmalle1
    dsb nsh
    isb
    mov x29, xzr
    mov x30, xzr
    mov x0, x9
    blr x8
3:  wfi
    b 3b
.ltorg
.size secondary_entry, . - secondary_entry

.section .bss.boot_stack, "aw", %nobits
.balign 16
.global boot_stack_bottom
boot_stack_bottom:
    .space 64 * 1024
.global boot_stack_top
boot_stack_top:
