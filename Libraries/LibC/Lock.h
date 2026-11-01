/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/thread.h>
#include <LibSystem/Syscall.h>

// The C library's own locks, and the futex operations under pthread's. A
// lock word is 0 when free, 1 when held, 2 when held with someone asleep on
// it (who must be woken on release); no system call unless there is
// contention.

namespace LibC {

inline long futex_wait(unsigned* word, unsigned expected, void const* timeout = nullptr)
{
    return System::syscall(System::Syscall::futex, reinterpret_cast<unsigned long>(word), VULTURE_FUTEX_WAIT, expected, reinterpret_cast<unsigned long>(timeout));
}

inline long futex_wake(unsigned* word, unsigned count)
{
    return System::syscall(System::Syscall::futex, reinterpret_cast<unsigned long>(word), VULTURE_FUTEX_WAKE, count);
}

inline bool try_lock_word(unsigned* word)
{
    unsigned expected = 0;
    return __atomic_compare_exchange_n(word, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

// Taken, or (with a timeout) the futex error that stopped it. A short spin
// first: most locks are held for less time than a system call takes.
void lock_word_slowly(unsigned* word);

inline void lock_word(unsigned* word)
{
    if (!try_lock_word(word))
        lock_word_slowly(word);
}

// Takes it marked as contended: for one who was asleep and may not be the
// last (a condition variable's waiter going back to its mutex).
inline void lock_word_contended(unsigned* word)
{
    while (__atomic_exchange_n(word, 2, __ATOMIC_ACQUIRE) != 0)
        futex_wait(word, 2);
}

inline void unlock_word(unsigned* word)
{
    if (__atomic_exchange_n(word, 0, __ATOMIC_RELEASE) == 2)
        futex_wake(word, 1);
}

class Lock {
public:
    void lock() { lock_word(&m_word); }
    void unlock() { unlock_word(&m_word); }
    bool try_lock() { return try_lock_word(&m_word); }
    // In a forked child, whose other threads are gone with whatever they held.
    void reset() { m_word = 0; }

private:
    unsigned m_word { 0 };
};

class LockGuard {
public:
    explicit LockGuard(Lock& lock)
        : m_lock(lock)
    {
        m_lock.lock();
    }
    ~LockGuard() { m_lock.unlock(); }
    LockGuard(LockGuard const&) = delete;
    LockGuard& operator=(LockGuard const&) = delete;

private:
    Lock& m_lock;
};

}
