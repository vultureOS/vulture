/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/ThreadContext.h>
#include <Kernel/Arch/x86_64/MMIO.h>

extern "C" void x86_64_switch_context(Kernel::Arch::ThreadContext*, Kernel::Arch::ThreadContext const*);
extern "C" u8 x86_64_thread_trampoline[];
extern "C" void x86_64_save_fp_state(u8*);

namespace Kernel::Arch {

static constexpr u32 msr_fs_base = 0xc000'0100;

// What the processor starts with: every exception masked, and the rest zero.
static constexpr u16 initial_fp_control = 0x037f;
static constexpr u32 initial_simd_control = 0x1f80;

void initialize_thread_context(ThreadContext& context, FlatPtr stack_top, ThreadEntry entry, void* argument)
{
    __builtin_memset(&context, 0, sizeof(context));
    context.rbx = reinterpret_cast<u64>(entry);
    context.r12 = reinterpret_cast<u64>(argument);
    __builtin_memcpy(context.fp_state, &initial_fp_control, sizeof(initial_fp_control));
    __builtin_memcpy(context.fp_state + 24, &initial_simd_control, sizeof(initial_simd_control));
    // What it returns to, where switch_context() expects to find that. At the top, the same alignment
    // as a call would have left it with.
    FlatPtr const stack = stack_top - 16;
    *reinterpret_cast<u64*>(stack) = reinterpret_cast<u64>(x86_64_thread_trampoline);
    context.stack_pointer = stack;
}

void switch_context(ThreadContext& from, ThreadContext const& to)
{
    x86_64_switch_context(&from, &to);
}

void set_user_thread_pointer(ThreadContext& context, FlatPtr pointer)
{
    context.thread_pointer = pointer;
}

void set_current_user_thread_pointer(FlatPtr pointer)
{
    write_msr(msr_fs_base, pointer);
}

void copy_current_user_state(ThreadContext& child)
{
    x86_64_save_fp_state(child.fp_state);
    child.thread_pointer = read_msr(msr_fs_base);
}

void enable_floating_point()
{
    // On, from the moment each processor was set up (CPU.cpp).
}

}
