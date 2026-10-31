/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>
#include <LibBase/FastRandom.h>

namespace Kernel {

namespace {

constexpr size_t timer_count = 64;

struct Run;

// What one timer saw when it fired. The timer's context points back here.
struct Record {
    Timer timer;
    Run* run { nullptr };
    u32 index { 0 };
    u64 fired_at { 0 };
    u32 fired_on { 0 };
    bool fired { false };
};

struct Run {
    Record records[timer_count];
    u32 order[timer_count] {};
    Atomic<u32> fired_count { 0 };
};

void record_firing(Timer& timer)
{
    auto& record = *static_cast<Record*>(timer.context);
    auto& run = *record.run;
    record.fired_at = Time::now();
    record.fired_on = Processor::current().id();
    record.fired = true;
    run.order[run.fired_count.fetch_add(1)] = record.index;
}

void prepare(Run& run, u32 index)
{
    auto& record = run.records[index];
    record.run = &run;
    record.index = index;
    record.timer.context = &record;
}

// Spins (interrupts stay enabled, so timers fire) until `count` timers have
// fired or the timeout passes.
bool wait_for_firings(Run const& run, u32 count, u64 timeout_milliseconds)
{
    u64 deadline = Time::now() + Time::ticks_from_milliseconds(timeout_milliseconds);
    while (run.fired_count.load() < count) {
        if (Time::now() > deadline)
            return false;
        Arch::pause();
    }
    return true;
}

}

KERNEL_TEST(timer, fires_in_deadline_order_and_never_early)
{
    static Run run;
    auto& queue = Processor::current().timer_queue();
    FastRandom random(2026);
    u64 now = Time::now();
    for (u32 i = 0; i < timer_count; ++i) {
        prepare(run, i);
        auto& record = run.records[i];
        queue.add(record.timer, now + Time::ticks_from_microseconds(random.next_between(500, 20'000)), record_firing);
    }
    KTEST_EXPECT(wait_for_firings(run, timer_count, 1000));

    u64 worst_lateness = 0;
    for (u32 i = 0; i < timer_count; ++i) {
        auto const& record = run.records[run.order[i]];
        KTEST_EXPECT(record.fired_at >= record.timer.deadline());
        if (i > 0)
            KTEST_EXPECT(run.records[run.order[i - 1]].timer.deadline() <= record.timer.deadline());
        worst_lateness = max(worst_lateness, record.fired_at - record.timer.deadline());
    }
    KTEST_EXPECT_EQ(queue.pending_count(), 0u);
    dmesgln("ktest:   {} timers, latest fired {} us after its deadline", timer_count, Time::microseconds_from_ticks(worst_lateness));
}

KERNEL_TEST(timer, cancelled_timers_never_fire)
{
    static Run run;
    auto& queue = Processor::current().timer_queue();
    u64 deadline = Time::now() + Time::ticks_from_milliseconds(5);
    constexpr u32 count = 10;
    for (u32 i = 0; i < count; ++i) {
        prepare(run, i);
        queue.add(run.records[i].timer, deadline + i, record_firing);
    }
    for (u32 i = 0; i < count; i += 2)
        KTEST_EXPECT(run.records[i].timer.cancel());
    KTEST_EXPECT(wait_for_firings(run, count / 2, 200));
    // Give a wrongly kept timer the chance to fire too.
    Time::wait_until(Time::now() + Time::ticks_from_milliseconds(10));
    KTEST_EXPECT_EQ(run.fired_count.load(), count / 2);
    for (u32 i = 0; i < count; ++i)
        KTEST_EXPECT_EQ(run.records[i].fired, i % 2 == 1);
    KTEST_EXPECT(!run.records[1].timer.cancel());
}

static void use_own_timer_queue(void*)
{
    constexpr u32 count = 16;
    Run run;
    auto& queue = Processor::current().timer_queue();
    u64 now = Time::now();
    for (u32 i = 0; i < count; ++i) {
        prepare(run, i);
        queue.add(run.records[i].timer, now + Time::ticks_from_microseconds(500 * (i + 1)), record_firing);
    }
    KTEST_EXPECT(wait_for_firings(run, count, 1000));
    for (u32 i = 0; i < count; ++i)
        KTEST_EXPECT_EQ(run.records[i].fired_on, Processor::current().id());
}

KERNEL_TEST(timer, every_processor_has_its_own_queue)
{
    Processor::run_on_all(use_own_timer_queue, nullptr);
}

KERNEL_TEST(timer, no_interrupts_without_timers)
{
    auto& processor = Processor::current();
    static Run run;
    prepare(run, 0);
    u64 interrupts_before = processor.interrupt_count();
    processor.timer_queue().add(run.records[0].timer, Time::now() + Time::ticks_from_milliseconds(50), record_firing);
    KTEST_EXPECT(wait_for_firings(run, 1, 500));
    // Tickless: 50 ms with one timer armed takes one interrupt, not a tick's worth.
    u64 interrupts = processor.interrupt_count() - interrupts_before;
    KTEST_EXPECT(interrupts >= 1 && interrupts <= 2);
    dmesgln("ktest:   {} interrupt(s) in 50 ms", interrupts);
}

}
