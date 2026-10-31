/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/TrapFrame.h>
#include <LibBase/Format.h>

#if defined(__aarch64__)
#include <Kernel/Arch/aarch64/CPUInlines.h>
#else
#error "Unsupported architecture"
#endif

// What the rest of the kernel may ask of the CPU. Each architecture
// implements these in Kernel/Arch/<arch>/; the hot ones (interrupt masking,
// the counter, the per-CPU pointer) are inline in <arch>/CPUInlines.h.

namespace Kernel::Arch {

// Stops this CPU for good, with interrupts masked.
[[noreturn]] void halt();

// Stops translating lower-half (user) addresses, so stray accesses near
// null fault instead of hitting the loader's leftover identity mapping.
void disable_lower_half_translation();

// The frequency of monotonic_counter(), in Hz.
[[nodiscard]] u64 monotonic_counter_frequency();

// "Apple core (part 0x...), EL1" and similar.
void describe_cpu(FormatSink&);

// Explains the exception in `frame` in one line.
void describe_exception(FormatSink&, TrapFrame const&, u64 vector_index);

// Prints the general-purpose registers.
void dump_registers(FormatSink&, TrapFrame const&);

// One frame-pointer record: where the caller's record is and where to return.
struct StackFrame {
    FlatPtr previous_frame_pointer;
    FlatPtr return_address;
};

}
