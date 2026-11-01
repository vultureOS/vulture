/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What the thread pointer (TPIDR_EL0) points at, as AArch64's ELF
// thread-local storage has it: two reserved words, then the thread's static
// TLS block, the program's module first. dyld (or the C library, in a
// statically linked program) sets it up for the first thread; pthread_create
// copies `image` for each new one.

namespace System {

// Every module's initial data, laid out as a thread's block is, starting at
// the thread pointer (the first 16 bytes are the control block's place).
struct ThreadLocalImage {
    void const* data;
    unsigned long size;
    unsigned long alignment;
};

struct ThreadControlBlock {
    ThreadLocalImage const* image;
    void* thread; // The C library's own (pthread_self()).
};

static_assert(sizeof(ThreadControlBlock) == 16);

inline ThreadControlBlock* thread_control_block()
{
    unsigned long value;
    asm volatile("mrs %0, tpidr_el0" : "=r"(value));
    return reinterpret_cast<ThreadControlBlock*>(value);
}

inline void set_thread_control_block(ThreadControlBlock* block)
{
    asm volatile("msr tpidr_el0, %0" : : "r"(block));
}

}
