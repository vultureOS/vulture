/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/Timer.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Time/TimerQueue.h>

namespace Kernel {

void TimerQueue::add(Timer& timer, u64 deadline, TimerCallback callback)
{
    InterruptDisabler disabler;
    VERIFY(this == &Processor::current().timer_queue());
    VERIFY(!timer.is_pending());
    VERIFY(callback);
    timer.m_deadline = deadline;
    timer.m_callback = callback;

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

bool TimerQueue::cancel(Timer& timer)
{
    InterruptDisabler disabler;
    VERIFY(this == &Processor::current().timer_queue());
    if (!m_timers.contains(timer))
        return false;
    bool was_first = m_timers.first() == &timer;
    m_timers.remove(timer);
    if (was_first)
        program_hardware();
    return true;
}

void TimerQueue::handle_interrupt()
{
    // One snapshot of the time: a callback that re-arms its timer for a
    // moment already past fires on the next interrupt, not in a loop here.
    u64 now = Arch::monotonic_counter();
    while (auto* timer = m_timers.first()) {
        if (timer->m_deadline > now)
            break;
        m_timers.remove(*timer);
        ++m_fired_count;
        timer->m_callback(*timer);
    }
    program_hardware();
}

void TimerQueue::program_hardware()
{
    if (auto* first = m_timers.first())
        Arch::set_timer_deadline(first->m_deadline);
    else
        Arch::stop_timer();
}

}
