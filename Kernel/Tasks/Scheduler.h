/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Tasks/Thread.h>

namespace Kernel {

class Processor;

// Decides which thread runs on each processor.
//
// Every processor has a run queue and an idle thread. A woken or new thread
// goes to an idle processor if there is one (its last one first), else back
// to its last processor. A processor that runs out of work takes a thread
// from another one's queue before it goes idle. The most urgent QoS class
// always runs first; threads of one class take turns in time slices, which
// are armed only while others wait, so a lone thread runs untimed.
//
// Switches happen when a thread blocks, yields or exits, and on the way out
// of an interrupt that asked for one (a time slice ended, or a more urgent
// thread woke up).
class Scheduler {
public:
    static constexpr u64 time_slice_microseconds = 4000;

    // On the boot processor, once interrupts and timers work: the code
    // running now becomes the "boot" thread (pinned here), and an idle
    // thread is made for this processor.
    static void initialize();

    // On a secondary processor: the code running now becomes its idle
    // thread. Does not return.
    [[noreturn]] static void run_secondary_processor();

    [[nodiscard]] static bool is_running() { return s_running.load(MemoryOrder::Acquire); }

    // Makes a new or woken thread runnable.
    static void enqueue(Thread&);
    static void wake(Thread&);
    // Wakes a thread to run next on this processor (direct handoff: for a
    // caller that is about to block waiting on it).
    static void wake_for_handoff(Thread&);

    // Called with `lock` held (interrupts masked as a result): marks the
    // current thread blocked, adds it to `waiters` (if given), releases
    // `lock` and switches away. Returns once woken, with interrupts
    // restored to `state`, the value lock() returned.
    static void block(Thread::WaitList* waiters, SpinLock* lock, Arch::InterruptState state);

    static void yield();

    // On the way out of an interrupt: switches threads if one was asked for.
    static void preempt_if_needed();
    // After waking threads from thread context: switches right away if a
    // woken thread is more urgent than the current one. Does nothing with
    // interrupts masked.
    static void reschedule_if_needed();

    // From the wakeup IPI: another processor queued work here.
    static void handle_wakeup_ipi();

    // A thread's rank for comparisons: 0 for idle, then the QoS classes.
    [[nodiscard]] static u8 rank(QoS qos) { return static_cast<u8>(to_underlying(qos) + 1); }

private:
    friend void ::kernel_thread_start(Arch::ThreadEntry, void*);

    static void schedule();
    static void switch_to(Processor&, Thread& current, Thread& next);
    static void finish_switch();
    static void reap(Thread&);
    static Thread* steal_work(Processor&);
    static Processor& choose_processor(Thread&);
    static void notify(Processor& target, Thread&);
    static void update_time_slice(Processor&);
    [[noreturn]] static void idle_loop();
    static void idle_thread_main(void*);

    static Atomic<bool> s_running;
};

}
