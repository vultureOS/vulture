/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Tasks/Thread.h>

namespace Kernel {

// One processor's runnable threads: a FIFO list per QoS class. Changes
// happen under `lock`; the counts may also be read without it, as hints.
class RunQueue {
public:
    SpinLock lock;

    void push(Thread&);
    // At the front of its class: next to run (direct handoff).
    void push_front(Thread&);
    // The first thread of the most urgent class.
    [[nodiscard]] Thread* pop_most_urgent();
    // For another processor: the last thread of the most urgent class that
    // is allowed to run on `processor_id`.
    [[nodiscard]] Thread* steal_for(u32 processor_id);
    // 0 when empty, otherwise the most urgent class's rank (see Scheduler).
    [[nodiscard]] u8 most_urgent_rank() const;

    [[nodiscard]] u32 count() const { return m_count.load(MemoryOrder::Relaxed); }
    // Threads that are not pinned here, which other processors may take.
    [[nodiscard]] u32 stealable_count() const { return m_stealable_count.load(MemoryOrder::Relaxed); }

private:
    friend class Scheduler;
    void remove(Thread&);

    Thread::WaitList m_classes[qos_class_count];
    Atomic<u32> m_count { 0 };
    Atomic<u32> m_stealable_count { 0 };
};

}
