/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Console.h>
#include <LibBase/Assertions.h>

// Runtime hooks the loader needs because it links no C or C++ runtime.

[[noreturn]] static void halt()
{
    for (;;) {
#if defined(__aarch64__)
        asm volatile("wfi");
#elif defined(__x86_64__)
        asm volatile("hlt");
#endif
    }
}

void Base::verification_failed(char const* expression, char const* file, unsigned line)
{
    if (auto* console = Boot::g_console) {
        console->set_color(UEFI::TextColor::LightRed);
        console->println("VultureBoot: VERIFY failed: {} at {}:{}", expression, file, line);
    }
    halt();
}

// Pure virtual slots in vtables point here.
extern "C" [[noreturn]] void __cxa_pure_virtual()
{
    VERIFY_NOT_REACHED();
}
