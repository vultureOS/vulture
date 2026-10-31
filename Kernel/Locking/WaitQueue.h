/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <Kernel/Time/Time.h>

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

    // Like wait_until(), but a signal for the waiting process ends the wait
    // early. Returns false then.
    template<typename Condition>
    [[nodiscard]] bool wait_until_interruptible(Condition condition)
    {
        auto& thread = Thread::current();
        thread.set_blocked_on(this);
        bool satisfied = false;
        for (;;) {
            auto state = m_lock.lock();
            if (condition()) {
                satisfied = true;
                m_lock.unlock(state);
                break;
            }
            // A signal sent after this check finds us on this queue (the
            // sender takes the queue's lock to wake it).
            if (thread.has_pending_signal()) {
                m_lock.unlock(state);
                break;
            }
            Scheduler::block(&m_waiters, &m_lock, state);
        }
        thread.set_blocked_on(nullptr);
        return satisfied;
    }

    enum class WaitResult : u8 {
        Satisfied,
        Interrupted, // By a signal.
        TimedOut,
    };

    // wait_until_interruptible() that also gives up at `deadline` (a
    // Time::now() value).
    template<typename Condition>
    [[nodiscard]] WaitResult wait_until_interruptible(Condition condition, u64 deadline)
    {
        auto& thread = Thread::current();
        thread.set_blocked_on(this);
        Timer timer;
        timer.context = this;
        bool armed = false;
        WaitResult result;
        for (;;) {
            auto state = m_lock.lock();
            if (condition()) {
                result = WaitResult::Satisfied;
                m_lock.unlock(state);
                break;
            }
            if (thread.has_pending_signal()) {
                result = WaitResult::Interrupted;
                m_lock.unlock(state);
                break;
            }
            if (Time::now() >= deadline) {
                result = WaitResult::TimedOut;
                m_lock.unlock(state);
                break;
            }
            if (!armed) {
                // Wakes everyone here; the others just look again.
                Processor::current().timer_queue().add(timer, deadline, [](Timer& expired) {
                    static_cast<WaitQueue*>(expired.context)->wake_all();
                });
                armed = true;
            }
            Scheduler::block(&m_waiters, &m_lock, state);
        }
        if (armed)
            timer.cancel();
        thread.set_blocked_on(nullptr);
        return result;
    }

    void wake_one();
    void wake_all();

private:
    SpinLock m_lock;
    Thread::WaitList m_waiters;
};

}
