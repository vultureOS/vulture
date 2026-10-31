/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/Timer.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Time/TimerQueue.h>

namespace Kernel {

void TimerQueue::add(Timer& timer, u64 deadline, TimerCallback callback)
{
    VERIFY(callback);
    SpinLockGuard guard(m_lock);
    VERIFY(this == &Processor::current().timer_queue());
    VERIFY(!timer.is_pending());
    timer.m_deadline = deadline;
    timer.m_callback = callback;
    timer.m_queue.store(this, MemoryOrder::Release);

    // Ordered by deadline; equal deadlines fire in the order they were added.
    Timer* later = nullptr;
    for (auto& queued : m_timers) {
        if (queued.m_deadline > deadline) {
            later = &queued;
            break;
        }
    }
    if (later)
        m_timers.insert_before(timer, *later);
    else
        m_timers.append(timer);
    if (m_timers.first() == &timer)
        program_hardware();
}

bool Timer::cancel()
{
    for (;;) {
        auto* queue = m_queue.load(MemoryOrder::Acquire);
        if (!queue)
            return false;
        bool local = false;
        {
            SpinLockGuard guard(queue->m_lock);
            local = queue == &Processor::current().timer_queue();
            if (queue->m_timers.contains(*this)) {
                bool was_first = queue->m_timers.first() == this;
                queue->m_timers.remove(*this);
                m_queue.store(nullptr, MemoryOrder::Release);
                // Only the owning processor can reprogram its hardware; on
                // another, the old deadline just fires early and finds nothing.
                if (was_first && local)
                    queue->program_hardware();
                return true;
            }
        }
        // Not queued, yet still claimed: its callback is running. A callback
        // cancelling its own timer must not wait for itself.
        if (local && Processor::current().is_in_interrupt() && queue->m_running.load() == this)
            return false;
        while (queue->m_running.load(MemoryOrder::Acquire) == this)
            Arch::pause();
    }
}

void TimerQueue::handle_interrupt()
{
    // One snapshot of the time: a callback that re-arms its timer for a
    // moment already past fires on the next interrupt, not in a loop here.
    u64 now = Arch::monotonic_counter();
    auto state = m_lock.lock();
    while (auto* timer = m_timers.first()) {
        if (timer->m_deadline > now)
            break;
        m_timers.remove(*timer);
        ++m_fired_count;
        m_running.store(timer, MemoryOrder::Release);
        // Without the lock, so the callback may arm timers (even this one).
        m_lock.unlock(state);
        timer->m_callback(*timer);
        state = m_lock.lock();
        if (!timer->is_pending())
            timer->m_queue.store(nullptr, MemoryOrder::Release);
        m_running.store(nullptr, MemoryOrder::Release);
    }
    program_hardware();
    m_lock.unlock(state);
}

void TimerQueue::program_hardware()
{
    if (auto* first = m_timers.first())
        Arch::set_timer_deadline(first->m_deadline);
    else
        Arch::stop_timer();
}

}
