/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/Panic.h>
#include <Kernel/Library/CxxRuntime.h>
#include <LibBase/Assertions.h>
#include <LibBase/Format.h>

// The parts of the C++ runtime the kernel needs, since it links none.

using Constructor = void (*)();
extern "C" Constructor __init_array_start[];
extern "C" Constructor __init_array_end[];

extern "C" {

// The kernel never exits, so destructors of global objects never need to
// run; registering them is a no-op.
int __cxa_atexit(void (*)(void*), void*, void*)
{
    return 0;
}

void* __dso_handle = nullptr;

[[noreturn]] void __cxa_pure_virtual()
{
    Kernel::panic("pure virtual function called"_sv);
}
}

void Base::verification_failed(char const* expression, char const* file, unsigned line)
{
    FixedStringBuilder<256> reason;
    format_to(reason, "VERIFY failed: {} at {}:{}", expression, file, line);
    Kernel::panic(reason.view());
}

namespace Kernel {

void run_global_constructors()
{
    for (Constructor* constructor = __init_array_start; constructor != __init_array_end; ++constructor)
        (*constructor)();
}

}
