/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Tasks/Scheduler.h>

namespace Kernel {

// Threads waiting for a condition to become true. The condition is checked
// under the queue's lock, and wake_one()/wake_all() take that lock, so a
// wakeup that happens after the condition changed is never missed:
//
//     queue.wait_until([&] { return done.load(); });   // waiter
//     done.store(true); queue.wake_all();             // waker
class WaitQueue {
public:
    WaitQueue() = default;
    WaitQueue(WaitQueue const&) = delete;
    WaitQueue& operator=(WaitQueue const&) = delete;

    template<typename Condition>
    void wait_until(Condition condition)
    {
        for (;;) {
            auto state = m_lock.lock();
            if (condition()) {
                m_lock.unlock(state);
                return;
            }
            Scheduler::block(&m_waiters, &m_lock, state);
        }
    }

    void wake_one();
    void wake_all();

private:
    SpinLock m_lock;
    Thread::WaitList m_waiters;
};

}
