/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// The hot parts of Kernel/Arch/CPU.h, inline. Include Kernel/Arch/CPU.h
// rather than this file.

namespace Kernel::Arch {

// The saved RFLAGS of this CPU.
using InterruptState = u64;

static constexpr u64 rflags_interrupt_enable = 1 << 9;

// Masks interrupts on this CPU and returns the previous state, for
// restore_interrupts().
[[nodiscard]] inline InterruptState disable_interrupts()
{
    InterruptState state;
    asm volatile("pushfq\n"
                 "popq %0\n"
                 "cli"
        : "=r"(state)::"memory");
    return state;
}

inline void restore_interrupts(InterruptState state)
{
    if (state & rflags_interrupt_enable)
        asm volatile("sti" ::: "memory");
}

inline void enable_interrupts()
{
    asm volatile("sti" ::: "memory");
}

// Masks interrupts when the previous state does not matter.
inline void mask_interrupts()
{
    asm volatile("cli" ::: "memory");
}

[[nodiscard]] inline bool are_interrupts_enabled()
{
    u64 flags;
    asm volatile("pushfq\n"
                 "popq %0"
        : "=r"(flags)::"memory");
    return flags & rflags_interrupt_enable;
}

// Sleeps until an interrupt is pending, and takes it. An interrupt can't arrive between sti and hlt,
// so "mask, check for work, wait, unmask" cannot miss a wakeup. (Unlike on Arm, the interrupt is
// handled here, rather than at the unmasking after this.)
inline void wait_for_interrupt()
{
    asm volatile("sti\n"
                 "hlt\n"
                 "cli" ::
                     : "memory");
}

// A hint for spin-wait loops.
inline void pause()
{
    asm volatile("pause" ::: "memory");
}

inline u64 monotonic_counter()
{
    // The lfence keeps the counter read from being hoisted above earlier code.
    u32 low, high;
    asm volatile("lfence\n"
                 "rdtsc"
        : "=a"(low), "=d"(high)::"memory");
    return (static_cast<u64>(high) << 32) | low;
}

// The per-CPU data pointer (Kernel::Processor): the first word of what GS points at, in the kernel.
[[nodiscard]] inline void* current_processor_pointer()
{
    void* processor;
    asm volatile("movq %%gs:0, %0" : "=r"(processor));
    return processor;
}

inline void set_current_processor_pointer(void* processor)
{
    asm volatile("movq %0, %%gs:0" ::"r"(processor) : "memory");
}

// This CPU's local APIC ID: how the firmware's tables and interrupts name it.
[[nodiscard]] inline u64 processor_hardware_id()
{
    u64 id;
    asm volatile("movq %%gs:32, %0" : "=r"(id));
    return id;
}

}
