/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(__aarch64__)
#include <Kernel/Arch/aarch64/ThreadContext.h>
#else
#error "Unsupported architecture"
#endif

namespace Kernel::Arch {

using ThreadEntry = void (*)(void* argument);

// Prepares a new thread: switching to `context` the first time runs
// kernel_thread_start(entry, argument) at `stack_top`.
void initialize_thread_context(ThreadContext&, FlatPtr stack_top, ThreadEntry, void* argument);

// Saves the current thread's state into `from` and resumes `to`. Returns
// when something later switches back to `from`. Interrupts must be masked.
void switch_context(ThreadContext& from, ThreadContext const& to);

// Lets the current processor execute FP/SIMD instructions (the context
// switch saves those registers).
void enable_floating_point();

}

// Where every thread begins (defined by the scheduler).
extern "C" [[noreturn]] void kernel_thread_start(Kernel::Arch::ThreadEntry, void* argument);
