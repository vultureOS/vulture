/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/CPU.h>
#include <LibBase/Atomic.h>

namespace Kernel {

// A fair (first come, first served) ticket lock that masks interrupts on the
// holding CPU. An interrupt handler can therefore never spin on a lock that
// the code it interrupted holds, which rules out that deadlock by design.
//
// Hold spin locks briefly and never sleep while holding one. Taking a lock
// the current CPU already holds is a bug and panics, as does waiting for one
// longer than a few seconds.
class SpinLock {
public:
    constexpr SpinLock() = default;
    SpinLock(SpinLock const&) = delete;
    SpinLock& operator=(SpinLock const&) = delete;

    // Returns the interrupt state to hand back to unlock().
    [[nodiscard]] Arch::InterruptState lock();
    void unlock(Arch::InterruptState);

    [[nodiscard]] bool is_locked() const { return m_owner.load(MemoryOrder::Relaxed) != 0; }
    [[nodiscard]] bool is_locked_by_current_processor() const { return m_owner.load(MemoryOrder::Relaxed) == owner_token(); }

private:
    // The owning CPU's hardware id plus one, so that 0 means "free".
    static u64 owner_token() { return Arch::processor_hardware_id() + 1; }

    [[gnu::noinline]] void wait_for_turn(u32 ticket);

    Atomic<u32> m_next_ticket { 0 };
    Atomic<u32> m_now_serving { 0 };
    Atomic<u64> m_owner { 0 };
};

class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& lock)
        : m_lock(lock)
        , m_interrupt_state(lock.lock())
    {
    }

    ~SpinLockGuard() { m_lock.unlock(m_interrupt_state); }

    SpinLockGuard(SpinLockGuard const&) = delete;
    SpinLockGuard& operator=(SpinLockGuard const&) = delete;

private:
    SpinLock& m_lock;
    Arch::InterruptState m_interrupt_state;
};

// Masks interrupts on this CPU for the lifetime of the object.
class InterruptDisabler {
public:
    InterruptDisabler()
        : m_state(Arch::disable_interrupts())
    {
    }

    ~InterruptDisabler() { Arch::restore_interrupts(m_state); }

    InterruptDisabler(InterruptDisabler const&) = delete;
    InterruptDisabler& operator=(InterruptDisabler const&) = delete;

private:
    Arch::InterruptState m_state;
};

}
