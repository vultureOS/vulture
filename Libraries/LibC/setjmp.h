/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

// x19-x30, sp and d8-d15: what a function call preserves.
typedef unsigned long long jmp_buf[22];
typedef unsigned long long sigjmp_buf[23];

int setjmp(jmp_buf);
__noreturn void longjmp(jmp_buf, int value);

__END_DECLS
