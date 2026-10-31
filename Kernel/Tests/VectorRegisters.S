/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * For the kernel tests: fill all 32 vector registers with a pattern, and
 * later count the registers that lost it. Kernel C++ never touches these
 * registers, so only a context switch that failed to save and restore them
 * could change them in between.
 */

.text

/* void ktest_fill_vector_registers(u64 seed): v<n> = { seed + n, ~(seed + n) } */
.global ktest_fill_vector_registers
.type ktest_fill_vector_registers, %function
ktest_fill_vector_registers:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    add x1, x0, #\n
    mvn x2, x1
    fmov d\n, x1
    mov v\n\().d[1], x2
.endr
    ret
.size ktest_fill_vector_registers, . - ktest_fill_vector_registers

/* u64 ktest_count_wrong_vector_registers(u64 seed): halves that differ */
.global ktest_count_wrong_vector_registers
.type ktest_count_wrong_vector_registers, %function
ktest_count_wrong_vector_registers:
    mov x3, #0
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    add x1, x0, #\n
    mvn x2, x1
    fmov x4, d\n
    mov x5, v\n\().d[1]
    cmp x4, x1
    cinc x3, x3, ne
    cmp x5, x2
    cinc x3, x3, ne
.endr
    mov x0, x3
    ret
.size ktest_count_wrong_vector_registers, . - ktest_count_wrong_vector_registers
