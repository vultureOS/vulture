/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/WaitQueue.h>

namespace Kernel {

void WaitQueue::wake_one()
{
    Thread* thread;
    {
        SpinLockGuard guard(m_lock);
        thread = m_waiters.take_first();
    }
    if (thread) {
        Scheduler::wake(*thread);
        Scheduler::reschedule_if_needed();
    }
}

void WaitQueue::wake_all()
{
    // Waking takes run queue locks, so move the waiters out first.
    Thread::WaitList woken;
    {
        SpinLockGuard guard(m_lock);
        while (auto* thread = m_waiters.take_first())
            woken.append(*thread);
    }
    if (woken.is_empty())
        return;
    while (auto* thread = woken.take_first())
        Scheduler::wake(*thread);
    Scheduler::reschedule_if_needed();
}

}
