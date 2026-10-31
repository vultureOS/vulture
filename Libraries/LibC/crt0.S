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
.type _start, %function
_start:
    mov x29, xzr
    mov x30, xzr
    mov x0, sp
    bl __libc_start
    brk #0
.size _start, . - _start
