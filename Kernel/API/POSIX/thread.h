/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Threads of one process, as thread_create() starts them, and the futex
// they wait on. A futex is a 32-bit word in the process's memory: waiters
// sleep while it holds what they expect; wakers wake them by its address.
// Futexes are private to a process.

struct vulture_thread_parameters {
    void* entry;          // The thread starts as entry(argument). It must not return.
    void* argument;
    void* stack_top;      // 16-byte aligned.
    void* thread_pointer; // TPIDR_EL0 (thread-local storage).
    // Receives the thread's id before it starts; set to zero, and woken as
    // a futex, when thread_exit() ends it. May be null.
    unsigned* tid_address;
};

// futex(address, op, value, timeout)
// Sleeps while *address == value: 0 when woken, EAGAIN if it did not hold,
// ETIMEDOUT after `timeout` (a relative vulture_timespec, or null), EINTR.
#define VULTURE_FUTEX_WAIT 0
// Wakes up to `value` sleepers; returns how many.
#define VULTURE_FUTEX_WAKE 1
