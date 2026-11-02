/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(__aarch64__)
#include <Kernel/Arch/aarch64/ThreadContext.h>
#elif defined(__x86_64__)
#include <Kernel/Arch/x86_64/ThreadContext.h>
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

// A new user thread's thread pointer (for thread-local storage).
void set_user_thread_pointer(ThreadContext&, FlatPtr);
// The current thread's: what exec starts a program with.
void set_current_user_thread_pointer(FlatPtr);
// For fork: the child starts with the registers the trap frame does not
// hold (FP/SIMD, the thread pointer) as the calling thread has them now.
void copy_current_user_state(ThreadContext& child);

}

// Where every thread begins (defined by the scheduler).
extern "C" [[noreturn]] void kernel_thread_start(Kernel::Arch::ThreadEntry, void* argument);
