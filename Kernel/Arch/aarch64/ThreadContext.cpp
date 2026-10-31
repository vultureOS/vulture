/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/ThreadContext.h>
#include <Kernel/Arch/aarch64/SystemRegisters.h>

extern "C" void aarch64_switch_context(Kernel::Arch::ThreadContext*, Kernel::Arch::ThreadContext const*);
extern "C" u8 aarch64_thread_trampoline[];

namespace Kernel::Arch {

void initialize_thread_context(ThreadContext& context, FlatPtr stack_top, ThreadEntry entry, void* argument)
{
    __builtin_memset(&context, 0, sizeof(context));
    context.x19_to_x28[0] = reinterpret_cast<u64>(entry);
    context.x19_to_x28[1] = reinterpret_cast<u64>(argument);
    // A zero frame pointer ends backtraces at the thread's start.
    context.frame_pointer = 0;
    context.link_register = reinterpret_cast<u64>(aarch64_thread_trampoline);
    context.stack_pointer = stack_top;
}

void switch_context(ThreadContext& from, ThreadContext const& to)
{
    aarch64_switch_context(&from, &to);
}

void enable_floating_point()
{
    // CPACR_EL1.FPEN = 0b11: no trapping of FP/SIMD at EL1 or EL0.
    constexpr u64 fpen = 3ull << 20;
    WRITE_SYSTEM_REGISTER(cpacr_el1, READ_SYSTEM_REGISTER(cpacr_el1) | fpen);
    AArch64::instruction_barrier();
}

}
