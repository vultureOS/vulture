/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>

namespace Kernel {

KERNEL_TEST(smp, every_listed_processor_is_online)
{
    KTEST_EXPECT_EQ(Processor::online_count(), Processor::listed_count());
    KTEST_EXPECT(Processor::online_count() > 1);
}

namespace {

Atomic<u64> s_seen;
Atomic<u32> s_inconsistent;

}

static void check_identity(void*)
{
    auto& processor = Processor::current();
    s_seen.fetch_or(1ull << processor.id());
    auto frame = reinterpret_cast<FlatPtr>(__builtin_frame_address(0));
    bool on_own_stack = frame >= processor.stack_bottom() && frame < processor.stack_top();
    if (processor.hardware_id() != Arch::processor_hardware_id() || !on_own_stack || !Arch::are_interrupts_enabled())
        s_inconsistent.fetch_add(1);
}

KERNEL_TEST(smp, run_on_all_reaches_every_processor)
{
    s_seen.store(0);
    s_inconsistent.store(0);
    Processor::run_on_all(check_identity, nullptr);
    u32 count = Processor::online_count();
    KTEST_EXPECT_EQ(s_seen.load(), count == 64 ? ~0ull : (1ull << count) - 1);
    KTEST_EXPECT_EQ(s_inconsistent.load(), 0u);
}

KERNEL_TEST(smp, ipi_round_trip)
{
    constexpr u32 rounds = 1000;
    u64 start = Time::now();
    for (u32 i = 0; i < rounds; ++i)
        Processor::run_on_all([](void*) { }, nullptr);
    u64 nanoseconds = Time::nanoseconds_from_ticks(Time::now() - start) / rounds;
    dmesgln("ktest:   wake {} processors and wait for all: {} ns per round", Processor::online_count() - 1, nanoseconds);
}

}
