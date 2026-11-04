/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <fenv.h>

extern "C" {

#if defined(__aarch64__)

namespace {

constexpr unsigned long long round_mask = 0xc00000;

unsigned long long read_fpcr()
{
    unsigned long long value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

void write_fpcr(unsigned long long value)
{
    asm volatile("msr fpcr, %0" : : "r"(value));
}

unsigned long long read_fpsr()
{
    unsigned long long value;
    asm volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}

void write_fpsr(unsigned long long value)
{
    asm volatile("msr fpsr, %0" : : "r"(value));
}

}

int feclearexcept(int excepts)
{
    write_fpsr(read_fpsr() & ~static_cast<unsigned long long>(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fegetexceptflag(fexcept_t* flags, int excepts)
{
    *flags = read_fpsr() & static_cast<unsigned long long>(excepts & FE_ALL_EXCEPT);
    return 0;
}

int feraiseexcept(int excepts)
{
    write_fpsr(read_fpsr() | static_cast<unsigned long long>(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fesetexceptflag(fexcept_t const* flags, int excepts)
{
    auto const mask = static_cast<unsigned long long>(excepts & FE_ALL_EXCEPT);
    write_fpsr((read_fpsr() & ~mask) | (*flags & mask));
    return 0;
}

int fetestexcept(int excepts)
{
    return static_cast<int>(read_fpsr() & static_cast<unsigned long long>(excepts & FE_ALL_EXCEPT));
}

int fegetround()
{
    return static_cast<int>(read_fpcr() & round_mask);
}

int fesetround(int mode)
{
    if (mode & ~static_cast<int>(round_mask))
        return 1;
    write_fpcr((read_fpcr() & ~round_mask) | static_cast<unsigned long long>(mode));
    return 0;
}

int fegetenv(fenv_t* environment)
{
    environment->__fpcr = read_fpcr();
    environment->__fpsr = read_fpsr();
    return 0;
}

int feholdexcept(fenv_t* environment)
{
    fegetenv(environment);
    write_fpsr(environment->__fpsr & ~static_cast<unsigned long long>(FE_ALL_EXCEPT));
    return 0;
}

int fesetenv(fenv_t const* environment)
{
    if (environment == FE_DFL_ENV) {
        write_fpcr(read_fpcr() & ~round_mask);
        write_fpsr(read_fpsr() & ~static_cast<unsigned long long>(FE_ALL_EXCEPT));
        return 0;
    }
    write_fpcr(environment->__fpcr);
    write_fpsr(environment->__fpsr);
    return 0;
}

int feupdateenv(fenv_t const* environment)
{
    int const raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(environment);
    feraiseexcept(raised);
    return 0;
}

#else

namespace {

constexpr unsigned round_mask = 0x6000;

unsigned read_mxcsr()
{
    unsigned value;
    asm volatile("stmxcsr %0" : "=m"(value));
    return value;
}

void write_mxcsr(unsigned value)
{
    asm volatile("ldmxcsr %0" : : "m"(value));
}

}

int feclearexcept(int excepts)
{
    write_mxcsr(read_mxcsr() & ~static_cast<unsigned>(excepts & FE_ALL_EXCEPT));
    asm volatile("fnclex");
    return 0;
}

int fegetexceptflag(fexcept_t* flags, int excepts)
{
    *flags = static_cast<fexcept_t>(read_mxcsr() & static_cast<unsigned>(excepts & FE_ALL_EXCEPT));
    return 0;
}

int feraiseexcept(int excepts)
{
    write_mxcsr(read_mxcsr() | static_cast<unsigned>(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fesetexceptflag(fexcept_t const* flags, int excepts)
{
    auto const mask = static_cast<unsigned>(excepts & FE_ALL_EXCEPT);
    write_mxcsr((read_mxcsr() & ~mask) | (*flags & mask));
    return 0;
}

int fetestexcept(int excepts)
{
    return static_cast<int>(read_mxcsr() & static_cast<unsigned>(excepts & FE_ALL_EXCEPT));
}

int fegetround()
{
    return static_cast<int>(read_mxcsr() & round_mask);
}

int fesetround(int mode)
{
    if (mode & ~static_cast<int>(round_mask))
        return 1;
    write_mxcsr((read_mxcsr() & ~round_mask) | static_cast<unsigned>(mode));
    return 0;
}

int fegetenv(fenv_t* environment)
{
    environment->__mxcsr = read_mxcsr();
    environment->__x87_control = 0;
    environment->__x87_status = 0;
    return 0;
}

int feholdexcept(fenv_t* environment)
{
    fegetenv(environment);
    write_mxcsr(environment->__mxcsr & ~static_cast<unsigned>(FE_ALL_EXCEPT));
    return 0;
}

int fesetenv(fenv_t const* environment)
{
    if (environment == FE_DFL_ENV) {
        write_mxcsr(0x1f80);
        return 0;
    }
    write_mxcsr(environment->__mxcsr);
    return 0;
}

int feupdateenv(fenv_t const* environment)
{
    int const raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(environment);
    feraiseexcept(raised);
    return 0;
}

#endif
}
