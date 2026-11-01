/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel::Arch {

// What a thread needs restored to resume where it called switch_context():
// the callee-saved registers and the stack, plus the whole FP/SIMD state.
// Kernel code never uses FP/SIMD (-mgeneral-regs-only), so the FP registers
// always hold the state of whichever user thread last ran here; saving them
// at every switch keeps that state with its thread. ContextSwitch.S relies
// on these offsets.
struct alignas(16) ThreadContext {
    u64 x19_to_x28[10];
    u64 frame_pointer; // x29
    u64 link_register; // x30: where switch_context() returns to.
    u64 stack_pointer;
    u64 fpcr;
    u64 fpsr;
    u64 thread_pointer; // TPIDR_EL0: the user's, for thread-local storage.
    u64 vector_registers[64]; // q0-q31
};

static_assert(__builtin_offsetof(ThreadContext, frame_pointer) == 80);
static_assert(__builtin_offsetof(ThreadContext, stack_pointer) == 96);
static_assert(__builtin_offsetof(ThreadContext, fpsr) == 112);
static_assert(__builtin_offsetof(ThreadContext, thread_pointer) == 120);
static_assert(__builtin_offsetof(ThreadContext, vector_registers) == 128);
static_assert(sizeof(ThreadContext) == 640);

}
