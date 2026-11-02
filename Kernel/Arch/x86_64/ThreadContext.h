/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel::Arch {

// What a thread needs restored to resume where it called switch_context(): the callee-saved
// registers and the stack, plus the whole SSE state. Kernel code never uses those registers
// (-mno-sse), so they always hold the state of whichever user thread last ran here; saving them at
// every switch keeps that state with its thread. Entry.S relies on these offsets.
struct alignas(16) ThreadContext {
    u64 rbx, rbp, r12, r13, r14, r15;
    u64 stack_pointer;
    u64 thread_pointer; // FS base: the user's, for thread-local storage.
    u8 fp_state[512];   // As fxsave64 writes it.
};

static_assert(__builtin_offsetof(ThreadContext, stack_pointer) == 48);
static_assert(__builtin_offsetof(ThreadContext, thread_pointer) == 56);
static_assert(__builtin_offsetof(ThreadContext, fp_state) == 64);
static_assert(sizeof(ThreadContext) == 576);

}
