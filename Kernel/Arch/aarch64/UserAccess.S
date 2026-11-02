/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Copying to and from user memory. Every instruction that touches user
 * memory is listed in the __ex_table section with a recovery address: if it
 * faults (and the page fault handler cannot map the page), the exception
 * handler resumes there instead of panicking, and the copy reports failure.
 */

.macro USER_ACCESS fixup, instruction:vararg
9999: \instruction
    .pushsection __ex_table, "a"
    .balign 8
    .quad 9999b, \fixup
    .popsection
.endm

.text

/*
 * With PAN on, the kernel can't touch user memory even by accident: these are the one place that
 * means to, and they step around it for just as long as they need. An exception in between finds
 * PAN on again, and on the way back it goes back to what it was here.
 */
.arch_extension pan

.macro USER_ACCESS_BEGIN
    adrp x5, aarch64_has_pan
    ldrb w5, [x5, :lo12:aarch64_has_pan]
    cbz w5, 9998f
    msr pan, #0
9998:
.endm

.macro USER_ACCESS_END
    adrp x5, aarch64_has_pan
    ldrb w5, [x5, :lo12:aarch64_has_pan]
    cbz w5, 9997f
    msr pan, #1
9997:
.endm

/* u64 arch_copy_user(void* destination, void const* source, u64 size): 0, or 1 on a fault. */
.global arch_copy_user
.type arch_copy_user, %function
arch_copy_user:
    USER_ACCESS_BEGIN
    cbz x2, 2f
    /* Eight bytes at a time while both are aligned, then bytes. */
    orr x3, x0, x1
    tst x3, #7
    b.ne 1f
3:  cmp x2, #8
    b.lo 1f
    USER_ACCESS 4f, ldr x3, [x1], #8
    USER_ACCESS 4f, str x3, [x0], #8
    sub x2, x2, #8
    cbnz x2, 3b
    b 2f
1:  USER_ACCESS 4f, ldrb w3, [x1], #1
    USER_ACCESS 4f, strb w3, [x0], #1
    subs x2, x2, #1
    b.ne 1b
2:  USER_ACCESS_END
    mov x0, #0
    ret
4:  USER_ACCESS_END
    mov x0, #1
    ret
.size arch_copy_user, . - arch_copy_user

/*
 * i64 arch_copy_user_string(char* destination, char const* source, u64 capacity):
 * the length (without the terminator) once a NUL was copied; -1 on a fault;
 * -2 if no NUL appeared within `capacity` bytes.
 */
.global arch_copy_user_string
.type arch_copy_user_string, %function
arch_copy_user_string:
    USER_ACCESS_BEGIN
    mov x4, #0
1:  cmp x4, x2
    b.hs 3f
    USER_ACCESS 4f, ldrb w3, [x1, x4]
    strb w3, [x0, x4]
    cbz w3, 2f
    add x4, x4, #1
    b 1b
2:  USER_ACCESS_END
    mov x0, x4
    ret
3:  USER_ACCESS_END
    mov x0, #-2
    ret
4:  USER_ACCESS_END
    mov x0, #-1
    ret
.size arch_copy_user_string, . - arch_copy_user_string
