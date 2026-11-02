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
 * Secondary processors released from a spin table start here: where firmware without PSCI parked
 * them, MMU and caches off, with nothing in x0. So this finds what they would have been given.
 */
.section .text, "ax"
.global spin_table_entry
.type spin_table_entry, %function
spin_table_entry:
    msr daifset, #0xf
    /* By the number this processor goes by. */
    mrs x0, mpidr_el1
    and x0, x0, #0xff
    adrp x1, spin_table_parameters
    add x1, x1, :lo12:spin_table_parameters
    ldr x0, [x1, x0, lsl #3]
    b secondary_entry
.size spin_table_entry, . - spin_table_entry

/*
 * Secondary processors start here, from PSCI CPU_ON or the above: MMU and caches off, x0 = physical address
 * of their StartupParameters (see SMP.cpp). They may be at EL2, as firmware may start them at the highest
 * level there is, which a Raspberry Pi's does (the boot processor came down in its loader), so first
 * they come down. Then everything is loaded into registers before the MMU goes on, because
 * memory reads now bypass the cache.
 */
.global secondary_entry
.type secondary_entry, %function
secondary_entry:
    msr daifset, #0xf
    mrs x1, CurrentEL
    lsr x1, x1, #2
    cmp x1, #2
    b.ne 5f
    movz x1, #0x8000, lsl #16
    orr x1, x1, #(3 << 40)  /* HCR_EL2: AArch64 below, pointer authentication not trapped */
    msr hcr_el2, x1
    mrs x1, cnthctl_el2
    orr x1, x1, #3          /* the counters and timers, for EL1 */
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    mov x1, #0x33ff         /* floating point, not trapped */
    msr cptr_el2, x1
    movz x1, #0x0838
    movk x1, #0x30d5, lsl #16 /* SCTLR_EL1: as a loader leaves it, but with translation off */
    msr sctlr_el1, x1
    mov x1, #0x3c5          /* EL1, on its own stack pointer, all masked */
    msr spsr_el2, x1
    adr x1, 5f
    msr elr_el2, x1
    eret
5:
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
