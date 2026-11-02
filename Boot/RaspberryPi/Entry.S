/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The first thing to run on a Raspberry Pi. Its firmware has loaded kernel8.img, whole, at some low address, and
 * jumps to the start of it: one processor, at EL2 (EL3 on an emulator that wants to show everything), with the MMU
 * off and x0 = the device tree. The other processors are parked in the firmware's spin table.
 *
 * This comes down to EL1, moves the code to where it was linked, if it isn't there (the firmware's choice has changed
 * over the years), and turns on the MMU with the first 1 GiB of memory cached, so that everything after it can be
 * ordinary C++ on fast memory.
 */

.section .text.entry, "ax"

.global _start
.type _start, %function
_start:
    /* The header Linux images have, as the firmware and QEMU look for it. */
    b early_start
    .word 0
    .quad 0x80000           /* Where it would like to go, from the start of memory. */
    .quad 0                 /* How big it is: mkpiimage puts that here. */
    .quad 0x2               /* Little-endian, 4 KiB pages. */
    .quad 0
    .quad 0
    .quad 0
    .word 0x644d5241        /* "ARM\x64" */
    .word 0

early_start:
    msr daifset, #0xf
    mov x19, x0             /* The device tree. */
    adr x20, _start         /* Where this really is. */

    mrs x0, CurrentEL
    ubfx x0, x0, #2, #2
    cmp x0, #3
    b.ne 1f
    mov x0, #0x5b1          /* SCR_EL3: non-secure, AArch64 below, HVC, no SMC. */
    msr scr_el3, x0
    mov x0, #0x3c9          /* EL2, on its own stack pointer, all masked. */
    msr spsr_el3, x0
    adr x0, at_el2
    msr elr_el3, x0
    eret
1:
    cmp x0, #2
    b.ne at_el1
at_el2:
    movz x0, #0x8000, lsl #16
    orr x0, x0, #(3 << 40)  /* HCR_EL2: AArch64 below, pointer authentication not trapped. */
    msr hcr_el2, x0
    mrs x0, cnthctl_el2
    orr x0, x0, #3          /* The counters and timers, for EL1. */
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    mov x0, #0x33ff         /* Floating point, not trapped. */
    msr cptr_el2, x0
    msr vttbr_el2, xzr
    movz x0, #0x0838
    movk x0, #0x30d5, lsl #16 /* SCTLR_EL1: what must be set, and WFI and WFE for programs; translation off. */
    msr sctlr_el1, x0
    mov x0, #0x3c5          /* EL1, on its own stack pointer, all masked. */
    msr spsr_el2, x0
    adr x0, at_el1
    msr elr_el2, x0
    eret

at_el1:
    /* Floating point, as the kernel expects from a loader. */
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    /* To where it was linked. It's only ever somewhere higher, so forwards is safe. */
    ldr x1, =_start
    cmp x1, x20
    b.eq moved
    ldr x2, =__image_end
    sub x2, x2, x1
    mov x3, x20
    mov x4, x1
2:
    ldp x5, x6, [x3], #16
    stp x5, x6, [x4], #16
    subs x2, x2, #16
    b.gt 2b
    dsb sy
    ic iallu
    dsb sy
    isb
    ldr x1, =moved
    br x1

moved:
    ldr x1, =__bss_start
    ldr x2, =__bss_end
3:
    cmp x1, x2
    b.hs 4f
    str xzr, [x1], #8
    b 3b
4:
    ldr x1, =__stack_top
    mov sp, x1

    /*
     * One table, of 1 GiB blocks: the first is memory, on every Pi. What it needs elsewhere is registers: from 1 GiB
     * to 4 GiB (a Pi 4's are at the top of that), and 64 GiB up (a Pi 5's). Memory there is left to the kernel.
     */
    ldr x1, =stub_translation_table
    mov x2, #0x705          /* Valid, a block, normal memory, inner shareable, accessed. */
    str x2, [x1]
    movz x2, #0x0401
    movk x2, #0x0060, lsl #48 /* Valid, a block, device memory, accessed, not executable. */
    mov x3, #0x40000000
    add x2, x2, x3
    str x2, [x1, #(1 * 8)]
    add x2, x2, x3
    str x2, [x1, #(2 * 8)]
    add x2, x2, x3
    str x2, [x1, #(3 * 8)]
    movz x4, #0x0401
    movk x4, #0x0010, lsl #32 /* 64 GiB. */
    movk x4, #0x0060, lsl #48
    mov x5, #64
5:
    str x4, [x1, x5, lsl #3]
    add x4, x4, x3
    add x5, x5, #1
    cmp x5, #128
    b.lo 5b

    mov x2, #0xff00         /* MAIR: 0 is device memory, 1 is normal, write-back. */
    msr mair_el1, x2
    mrs x3, id_aa64mmfr0_el1
    and x3, x3, #7
    lsl x3, x3, #32
    movz x2, #0x3519        /* 39-bit addresses from this table, cached walks, 4 KiB. */
    movk x2, #0x0080, lsl #16 /* Nothing from the other. */
    orr x2, x2, x3
    msr tcr_el1, x2
    msr ttbr0_el1, x1
    isb
    tlbi vmalle1
    dsb nsh
    isb
    mrs x2, sctlr_el1
    orr x2, x2, #(1 << 0)   /* MMU */
    orr x2, x2, #(1 << 2)   /* data cache */
    orr x2, x2, #(1 << 12)  /* instruction cache */
    msr sctlr_el1, x2
    isb

    mov x29, xzr
    mov x30, xzr
    mov x0, x19
    mov x1, x20
    bl stub_main
6:
    wfi
    b 6b
.ltorg
.size _start, . - _start
