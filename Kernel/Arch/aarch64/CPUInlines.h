/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/aarch64/SystemRegisters.h>

// The hot parts of Kernel/Arch/CPU.h, inline. Include Kernel/Arch/CPU.h
// rather than this file.

namespace Kernel::Arch {

// The saved interrupt mask (DAIF) of this CPU.
using InterruptState = u64;

static constexpr u64 daif_irq_masked = 1 << 7;

// Masks IRQs on this CPU and returns the previous state, for
// restore_interrupts().
[[nodiscard]] inline InterruptState disable_interrupts()
{
    InterruptState state;
    asm volatile("mrs %0, daif\n"
                 "msr daifset, #2"
        : "=r"(state)::"memory");
    return state;
}

inline void restore_interrupts(InterruptState state)
{
    asm volatile("msr daif, %0" ::"r"(state) : "memory");
}

inline void enable_interrupts()
{
    asm volatile("msr daifclr, #2" ::: "memory");
}

// Masks IRQs when the previous state does not matter.
inline void mask_interrupts()
{
    asm volatile("msr daifset, #2" ::: "memory");
}

[[nodiscard]] inline bool are_interrupts_enabled()
{
    return !(READ_SYSTEM_REGISTER(daif) & daif_irq_masked);
}

// Sleeps until an interrupt is pending. It wakes even with IRQs masked, so
// "mask, check for work, wait, unmask" cannot miss a wakeup.
inline void wait_for_interrupt()
{
    asm volatile("dsb sy\n"
                 "wfi" ::
                     : "memory");
}

// A hint for spin-wait loops.
inline void pause()
{
    asm volatile("yield" ::: "memory");
}

inline u64 monotonic_counter()
{
    // The ISB keeps the counter read from being hoisted above earlier code.
    asm volatile("isb" ::: "memory");
    return READ_SYSTEM_REGISTER(cntvct_el0);
}

// The per-CPU data pointer (Kernel::Processor), kept in TPIDR_EL1.
[[nodiscard]] inline void* current_processor_pointer()
{
    return reinterpret_cast<void*>(READ_SYSTEM_REGISTER(tpidr_el1));
}

inline void set_current_processor_pointer(void* processor)
{
    WRITE_SYSTEM_REGISTER(tpidr_el1, reinterpret_cast<FlatPtr>(processor));
}

// This CPU's affinity (MPIDR_EL1 without its flag bits): Aff3 in bits
// 39:32, Aff2/Aff1/Aff0 in bits 23:0. The firmware and the device tree name
// CPUs by this number.
[[nodiscard]] inline u64 processor_hardware_id()
{
    return READ_SYSTEM_REGISTER(mpidr_el1) & 0xff'00ff'ffffull;
}

}
