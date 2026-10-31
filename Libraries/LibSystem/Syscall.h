/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/Syscall.h>

// Raw system calls: SVC #0, number in x8, arguments in x0-x5, result in x0
// (a negative errno on failure). The kernel preserves every other register.

namespace System {

using Kernel::API::Syscall;

inline long syscall(Syscall number, unsigned long a0 = 0, unsigned long a1 = 0, unsigned long a2 = 0, unsigned long a3 = 0, unsigned long a4 = 0, unsigned long a5 = 0)
{
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
