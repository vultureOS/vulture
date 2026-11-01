/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Where the kernel starts a dynamically linked program: here, with the
// program's initial stack (argc, argv, environ, the auxiliary vector).
// dyld_main loads and links everything and returns the program's entry
// point, which then starts with that same stack, as if nothing came first.

.text
.global _dyld_start
.type _dyld_start, %function
_dyld_start:
    mov x29, xzr
    mov x30, xzr
    mov x0, sp
    bl dyld_main
    mov x16, x0
    mov x0, xzr
    mov x29, xzr
    mov x30, xzr
    br x16
.size _dyld_start, . - _dyld_start
