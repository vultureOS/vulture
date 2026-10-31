/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Locking/SpinLock.h>
#include <LibBase/IntrusiveList.h>

namespace Kernel {

class Timer;
class TimerQueue;
using TimerCallback = void (*)(Timer&);

// A one-shot callback at a point in time (a monotonic_counter() value).
// The callback runs in interrupt context on the processor that armed it.
class Timer {
public:
    Timer() = default;
    Timer(Timer const&) = delete;
    Timer& operator=(Timer const&) = delete;

    [[nodiscard]] bool is_pending() const { return m_list_node.is_in_list(); }
    [[nodiscard]] u64 deadline() const { return m_deadline; }

    // Disarms the timer, from any processor. Returns true if it was still
    // waiting. Afterwards its callback is neither running nor going to run:
    // if it is running right now, this waits for it to finish.
    bool cancel();

    void* context { nullptr };

private:
    friend class TimerQueue;

    IntrusiveListNode m_list_node;
    u64 m_deadline { 0 };
    TimerCallback m_callback { nullptr };
    // The queue it is armed on (or whose interrupt is running it).
    Atomic<TimerQueue*> m_queue { nullptr };
};

// One per processor, ordered by deadline. The hardware timer is armed for
// the earliest deadline only, and not at all while the queue is empty:
// there is no periodic tick.
class TimerQueue {
public:
    // Arms `timer` on the current processor. It must not be pending.
    void add(Timer&, u64 deadline, TimerCallback);

    // From the timer interrupt: runs every expired timer.
    void handle_interrupt();

    [[nodiscard]] size_t pending_count() const { return m_timers.size(); }
    [[nodiscard]] u64 fired_count() const { return m_fired_count; }

private:
    friend class Timer;

    void program_hardware();

    SpinLock m_lock;
    IntrusiveList<Timer, &Timer::m_list_node> m_timers;
    Atomic<Timer*> m_running { nullptr };
    u64 m_fired_count { 0 };
};

}
