/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/Panic.h>
#include <Kernel/Locking/SpinLock.h>

namespace Kernel {

// Far longer than any lock should be held, even with the host descheduling
// a virtual CPU now and then.
static constexpr u64 deadlock_timeout_seconds = 5;

Arch::InterruptState SpinLock::lock()
{
    auto state = Arch::disable_interrupts();
    if (is_locked_by_current_processor()) [[unlikely]]
        panic("SpinLock: this processor already holds the lock"_sv);

    u32 ticket = m_next_ticket.fetch_add(1, MemoryOrder::Relaxed);
    if (m_now_serving.load(MemoryOrder::Acquire) != ticket) [[unlikely]]
        wait_for_turn(ticket);
    m_owner.store(owner_token(), MemoryOrder::Relaxed);
    return state;
}

void SpinLock::wait_for_turn(u32 ticket)
{
    u64 start = Arch::monotonic_counter();
    u64 timeout = deadlock_timeout_seconds * Arch::monotonic_counter_frequency();
    u32 spins = 0;
    while (m_now_serving.load(MemoryOrder::Acquire) != ticket) {
        Arch::pause();
        // Reading the counter costs more than a pause; check it only now and then.
        if (++spins % 4096 == 0 && Arch::monotonic_counter() - start > timeout) [[unlikely]] {
            FixedStringBuilder<128> reason;
            format_to(reason, "SpinLock: waited {} s for a lock held by processor {:#x}", deadlock_timeout_seconds, m_owner.load(MemoryOrder::Relaxed) - 1);
            panic(reason.view());
        }
    }
}

void SpinLock::unlock(Arch::InterruptState state)
{
    if (!is_locked_by_current_processor()) [[unlikely]]
        panic("SpinLock: unlocking a lock this processor does not hold"_sv);
    m_owner.store(0, MemoryOrder::Relaxed);
    // Only the holder writes m_now_serving, so a plain increment is safe;
    // the release store publishes everything done under the lock.
    m_now_serving.store(m_now_serving.load(MemoryOrder::Relaxed) + 1, MemoryOrder::Release);
    Arch::restore_interrupts(state);
}

}
