/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>

namespace Kernel {

KERNEL_TEST(spinlock, masks_interrupts_while_held)
{
    SpinLock lock;
    KTEST_EXPECT(Arch::are_interrupts_enabled());
    {
        SpinLockGuard guard(lock);
        KTEST_EXPECT(!Arch::are_interrupts_enabled());
        KTEST_EXPECT(lock.is_locked_by_current_processor());
    }
    KTEST_EXPECT(Arch::are_interrupts_enabled());
    KTEST_EXPECT(!lock.is_locked());

    // Unlocking restores the earlier state, not "enabled".
    {
        InterruptDisabler disabler;
        {
            SpinLockGuard guard(lock);
        }
        KTEST_EXPECT(!Arch::are_interrupts_enabled());
    }
    KTEST_EXPECT(Arch::are_interrupts_enabled());
}

namespace {

constexpr u64 increments_per_processor = 100'000;

struct Shared {
    SpinLock lock;
    u64 counter { 0 };
    Atomic<u32> inside { 0 };
    Atomic<u32> overlaps { 0 };
};

Shared s_shared;

}

static void hammer_lock(void*)
{
    for (u64 i = 0; i < increments_per_processor; ++i) {
        SpinLockGuard guard(s_shared.lock);
        if (s_shared.inside.fetch_add(1, MemoryOrder::Relaxed) != 0)
            s_shared.overlaps.fetch_add(1, MemoryOrder::Relaxed);
        // A plain read-modify-write: only the lock keeps it exact.
        s_shared.counter = s_shared.counter + 1;
        s_shared.inside.fetch_sub(1, MemoryOrder::Relaxed);
    }
}

KERNEL_TEST(spinlock, mutual_exclusion_across_processors)
{
    s_shared.counter = 0;
    s_shared.overlaps.store(0);
    Processor::run_on_all(hammer_lock, nullptr);
    KTEST_EXPECT_EQ(s_shared.counter, increments_per_processor * Processor::online_count());
    KTEST_EXPECT_EQ(s_shared.overlaps.load(), 0u);
}

}
