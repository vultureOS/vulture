/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/Panic.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>

namespace Kernel {

static void verify_may_sleep()
{
    if (!Arch::are_interrupts_enabled() || Processor::current().is_in_interrupt()) [[unlikely]]
        panic("sleeping lock taken with interrupts masked or in an interrupt"_sv);
}

void Mutex::lock()
{
    // Whoever has it isn't coming back.
    if (is_panicking())
        return;
    verify_may_sleep();
    auto& self = Thread::current();
    auto state = m_lock.lock();
    if (m_owner == &self) [[unlikely]]
        panic("Mutex: this thread already holds it"_sv);
    if (!m_owner) {
        m_owner = &self;
        m_lock.unlock(state);
        return;
    }
    Scheduler::block(&m_waiters, &m_lock, state);
    // unlock() made us the owner before waking us.
    VERIFY(m_owner == &self);
}

bool Mutex::try_lock()
{
    auto& self = Thread::current();
    SpinLockGuard guard(m_lock);
    if (m_owner)
        return false;
    m_owner = &self;
    return true;
}

void Mutex::unlock()
{
    if (is_panicking())
        return;
    auto state = m_lock.lock();
    if (m_owner != &Thread::current()) [[unlikely]]
        panic("Mutex: unlocked by a thread that does not hold it"_sv);
    Thread* next = m_waiters.take_first();
    m_owner = next;
    m_lock.unlock(state);
    if (next) {
        Scheduler::wake(*next);
        Scheduler::reschedule_if_needed();
    }
}

bool Mutex::is_locked_by_current_thread() const
{
    SpinLockGuard guard(m_lock);
    return m_owner == &Thread::current();
}

void Semaphore::down()
{
    verify_may_sleep();
    auto state = m_lock.lock();
    if (m_count > 0) {
        --m_count;
        m_lock.unlock(state);
        return;
    }
    // up() hands its unit straight to us.
    Scheduler::block(&m_waiters, &m_lock, state);
}

bool Semaphore::try_down()
{
    SpinLockGuard guard(m_lock);
    if (m_count == 0)
        return false;
    --m_count;
    return true;
}

void Semaphore::up()
{
    auto state = m_lock.lock();
    Thread* next = m_waiters.take_first();
    if (!next)
        ++m_count;
    m_lock.unlock(state);
    if (next) {
        Scheduler::wake(*next);
        Scheduler::reschedule_if_needed();
    }
}

}
