/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

// What the processor keeps about floating point: how it rounds, and what's gone wrong so far.
#if defined(__aarch64__)
typedef struct {
    unsigned long long __fpcr;
    unsigned long long __fpsr;
} fenv_t;
typedef unsigned long long fexcept_t;
#    define FE_INVALID 0x01
#    define FE_DIVBYZERO 0x02
#    define FE_OVERFLOW 0x04
#    define FE_UNDERFLOW 0x08
#    define FE_INEXACT 0x10
#    define FE_TONEAREST 0x000000
#    define FE_UPWARD 0x400000
#    define FE_DOWNWARD 0x800000
#    define FE_TOWARDZERO 0xc00000
#else
typedef struct {
    unsigned int __mxcsr;
    unsigned short __x87_control;
    unsigned short __x87_status;
} fenv_t;
typedef unsigned short fexcept_t;
#    define FE_INVALID 0x01
#    define FE_DIVBYZERO 0x04
#    define FE_OVERFLOW 0x08
#    define FE_UNDERFLOW 0x10
#    define FE_INEXACT 0x20
#    define FE_TONEAREST 0x0000
#    define FE_DOWNWARD 0x2000
#    define FE_UPWARD 0x4000
#    define FE_TOWARDZERO 0x6000
#endif
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)
#define FE_DFL_ENV ((fenv_t const*)-1)

int feclearexcept(int);
int fegetexceptflag(fexcept_t*, int);
int feraiseexcept(int);
int fesetexceptflag(fexcept_t const*, int);
int fetestexcept(int);
int fegetround(void);
int fesetround(int);
int fegetenv(fenv_t*);
int feholdexcept(fenv_t*);
int fesetenv(fenv_t const*);
int feupdateenv(fenv_t const*);

__END_DECLS
