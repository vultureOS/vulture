/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/Panic.h>
#include <Kernel/Security/Random.h>
#include <Kernel/Security/StackProtector.h>

// What functions with arrays on their stacks put between those and the return address, and check
// before returning (-fstack-protector-strong). Until the random generator is up it has this
// value, which is only as good as nobody knowing it; so little runs before then.

extern "C" {

FlatPtr __stack_chk_guard = 0x00dead5ca1ab1e00;

[[noreturn]] void __stack_chk_fail()
{
    Kernel::panic("stack smashing detected: a function's frame was overwritten"_sv);
}
}

namespace Kernel {

[[gnu::no_stack_protector]] void initialize_stack_protector()
{
    FlatPtr guard;
    get_random_bytes({ reinterpret_cast<u8*>(&guard), sizeof(guard) });
    // A zero byte stops anything overflowing with a string from reading or copying it.
    __stack_chk_guard = guard & ~static_cast<FlatPtr>(0xff);
}

}
