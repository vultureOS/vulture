/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/Syscall.h>

// Raw system calls, with the result (a negative errno on failure) in the first register. The
// kernel preserves every other register, apart from what the instruction itself takes.
//
//   AArch64: SVC #0, number in x8, arguments in x0-x5.
//   x86_64: syscall, number in rax, arguments in rdi, rsi, rdx, r10, r8 and r9. It takes rcx and r11.

namespace System {

using Kernel::API::Syscall;

inline long syscall(Syscall number, unsigned long a0 = 0, unsigned long a1 = 0, unsigned long a2 = 0, unsigned long a3 = 0, unsigned long a4 = 0, unsigned long a5 = 0)
{
#if defined(__x86_64__)
    unsigned long result = static_cast<unsigned long>(number);
    register unsigned long r10 asm("r10") = a3;
    register unsigned long r8 asm("r8") = a4;
    register unsigned long r9 asm("r9") = a5;
    asm volatile("syscall"
        : "+a"(result)
        : "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return static_cast<long>(result);
#else
    register unsigned long x8 asm("x8") = static_cast<unsigned long>(number);
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x1 asm("x1") = a1;
    register unsigned long x2 asm("x2") = a2;
    register unsigned long x3 asm("x3") = a3;
    register unsigned long x4 asm("x4") = a4;
    register unsigned long x5 asm("x5") = a5;
    asm volatile("svc #0"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
        : "memory");
    return static_cast<long>(x0);
#endif
}

template<typename T>
unsigned long argument(T value)
{
    if constexpr (__is_pointer(T))
        return reinterpret_cast<unsigned long>(value);
    else
        return static_cast<unsigned long>(value);
}

}
