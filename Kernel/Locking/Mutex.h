/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Tasks/Thread.h>

namespace Kernel {

// A sleeping lock for threads: waiting puts the thread to sleep instead of
// spinning, so it may be held across blocking operations. Unlocking hands
// the lock straight to the longest waiter, which is fair and avoids a herd
// of woken threads racing for it. Not recursive; never from interrupts.
class Mutex {
public:
    Mutex() = default;
    Mutex(Mutex const&) = delete;
    Mutex& operator=(Mutex const&) = delete;

    void lock();
    [[nodiscard]] bool try_lock();
    void unlock();

    [[nodiscard]] bool is_locked_by_current_thread() const;

private:
    mutable SpinLock m_lock;
    Thread* m_owner { nullptr };
    Thread::WaitList m_waiters;
};

class MutexLocker {
public:
    explicit MutexLocker(Mutex& mutex)
        : m_mutex(mutex)
    {
        m_mutex.lock();
    }

    ~MutexLocker() { m_mutex.unlock(); }

    MutexLocker(MutexLocker const&) = delete;
    MutexLocker& operator=(MutexLocker const&) = delete;

private:
    Mutex& m_mutex;
};

// Counts available units: down() takes one, sleeping until there is one;
// up() returns one, handing it to the longest waiter if any.
class Semaphore {
public:
    explicit Semaphore(u32 initial_count = 0)
        : m_count(initial_count)
    {
    }

    Semaphore(Semaphore const&) = delete;
    Semaphore& operator=(Semaphore const&) = delete;

    void down();
    [[nodiscard]] bool try_down();
    void up();

private:
    SpinLock m_lock;
    u32 m_count { 0 };
    Thread::WaitList m_waiters;
};

}
