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
// As VULTURE_FUTEX_WAIT, for a lock that thread `holder` (the fifth argument) has: while this waits, that thread goes at least as fast as this one.
#define VULTURE_FUTEX_WAIT_FOR_HOLDER 2
// As VULTURE_FUTEX_WAIT, and ECANCELED if pthread_cancel() gets there first: for what's a cancellation point, but has something to tidy up.
#define VULTURE_FUTEX_WAIT_CANCELLABLE 3

// set_thread_qos(): one of these, and what it was back. As Scheduler's classes, from the lowest.
#define VULTURE_QOS_BACKGROUND 0
#define VULTURE_QOS_UTILITY 1
#define VULTURE_QOS_DEFAULT 2
#define VULTURE_QOS_USER_INITIATED 3
#define VULTURE_QOS_USER_INTERACTIVE 4
#define VULTURE_QOS_JUST_ASKING 255

// thread_cancel_control(what, value)
#define VULTURE_CANCEL_THREAD 0  // The thread with this ID, of this process.
#define VULTURE_CANCEL_ENABLE 1  // Can this thread be cancelled?
#define VULTURE_CANCEL_ASYNC 2   // At any moment, or only at a cancellation point?
#define VULTURE_CANCEL_HANDLER 3 // Where a cancelled thread goes.
