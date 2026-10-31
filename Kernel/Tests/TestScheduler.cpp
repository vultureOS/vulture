/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/Mutex.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>

namespace Kernel {

static NonnullRefPtr<Thread> start_thread(StringView name, ThreadEntry entry, void* argument, ThreadOptions options = {})
{
    auto thread = Thread::create(name, entry, argument, options);
    VERIFY(!thread.is_error());
    auto started = thread.release_value();
    started->start();
    return started;
}

static u32 spare_processor(u32 preferred)
{
    return Processor::online_count() > preferred ? preferred : 0;
}

namespace {

struct Spinner {
    Atomic<bool>* stop;
    u64 iterations { 0 };
};

void spin_until_stopped(void* argument)
{
    auto& spinner = *static_cast<Spinner*>(argument);
    while (!spinner.stop->load(MemoryOrder::Relaxed))
        ++spinner.iterations;
}

}

KERNEL_TEST(scheduler, time_slices_share_a_processor)
{
    // Two threads that never yield, on one processor: only time slices let
    // both run.
    u32 processor_id = spare_processor(2);
    auto& processor = Processor::with_id(processor_id);
    Atomic<bool> stop { false };
    Spinner spinners[2] = { { &stop }, { &stop } };
    u64 switches_before = processor.context_switch_count();
    RefPtr<Thread> threads[2];
    for (u32 i = 0; i < 2; ++i)
        threads[i] = start_thread("spinner"_sv, spin_until_stopped, &spinners[i], { .qos = QoS::Default, .pinned_processor = processor_id });
    Thread::sleep_for_microseconds(100'000);
    stop.store(true);
    for (auto& thread : threads)
        thread->join();

    u64 switches = processor.context_switch_count() - switches_before;
    for (auto const& spinner : spinners)
        KTEST_EXPECT(spinner.iterations > 0);
    // 100 ms in 4 ms slices: about 25 switches; well above a handful.
    KTEST_EXPECT(switches >= 10);
    u64 total = spinners[0].iterations + spinners[1].iterations;
    u64 share = total ? spinners[0].iterations * 100 / total : 0;
    KTEST_EXPECT(share >= 20 && share <= 80);
    dmesgln("ktest:   {} switches in 100 ms; the first spinner got {}% of the processor", switches, share);
}

namespace {

struct PreemptionRun {
    Atomic<bool> low_running { false };
    Atomic<bool> released { false };
    u64 high_started { 0 };
    u64 high_ran { 0 };
};

}

KERNEL_TEST(scheduler, more_urgent_thread_preempts_at_once)
{
    u32 processor_id = spare_processor(3);
    PreemptionRun run;
    auto low = start_thread("background"_sv, [](void* argument) {
        auto& state = *static_cast<PreemptionRun*>(argument);
        state.low_running.store(true);
        while (!state.released.load(MemoryOrder::Relaxed)) {
        }
    },
        &run, { .qos = QoS::Background, .pinned_processor = processor_id });
    while (!run.low_running.load())
        Thread::sleep_for_microseconds(100);

    run.high_started = Time::now();
    auto high = start_thread("interactive"_sv, [](void* argument) {
        auto& state = *static_cast<PreemptionRun*>(argument);
        state.high_ran = Time::now();
        state.released.store(true);
    },
        &run, { .qos = QoS::UserInteractive, .pinned_processor = processor_id });
    high->join();
    low->join();

    u64 latency = Time::microseconds_from_ticks(run.high_ran - run.high_started);
    // Well within a time slice: it must not wait for the spinner's turn to end.
    KTEST_EXPECT(latency < Scheduler::time_slice_microseconds);
    dmesgln("ktest:   a user-interactive thread ran {} us after it was started", latency);
}

namespace {

Atomic<u64> s_processors_used;

void busy_for_ten_milliseconds(void*)
{
    u64 deadline = Time::now() + Time::ticks_from_milliseconds(10);
    while (Time::now() < deadline) {
        InterruptDisabler disabler;
        s_processors_used.fetch_or(1ull << Processor::current().id(), MemoryOrder::Relaxed);
    }
}

}

KERNEL_TEST(scheduler, work_spreads_over_every_processor)
{
    s_processors_used.store(0);
    constexpr u32 count = 64;
    RefPtr<Thread> threads[count];
    for (auto& thread : threads)
        thread = start_thread("busy"_sv, busy_for_ten_milliseconds, nullptr);
    for (auto& thread : threads)
        thread->join();
    u32 used = __builtin_popcountll(s_processors_used.load());
    KTEST_EXPECT_EQ(used, Processor::online_count());
}

namespace {

// Two threads passing a "ball" back and forth through two semaphores.
struct Pair {
    Semaphore ping { 1 };
    Semaphore pong { 0 };
    Atomic<u64> rounds { 0 };
};

Atomic<bool> s_stop_pairs;

void ping_thread(void* argument)
{
    auto& pair = *static_cast<Pair*>(argument);
    for (;;) {
        pair.ping.down();
        if (s_stop_pairs.load(MemoryOrder::Relaxed)) {
            pair.pong.up();
            return;
        }
        pair.rounds.fetch_add(1, MemoryOrder::Relaxed);
        pair.pong.up();
    }
}

void pong_thread(void* argument)
{
    auto& pair = *static_cast<Pair*>(argument);
    for (;;) {
        pair.pong.down();
        if (s_stop_pairs.load(MemoryOrder::Relaxed)) {
            pair.ping.up();
            return;
        }
        pair.ping.up();
    }
}

}

// The M3 soak: 1000 threads ping-ponging across every processor, then all
// of them stopped and joined. A hang shows up as a run that never ends.
KERNEL_TEST(scheduler, thousand_threads_ping_pong)
{
    constexpr u32 pair_count = 500;
    auto* pairs = new (Base::nothrow) Pair[pair_count];
    KTEST_EXPECT(pairs);
    if (!pairs)
        return;
    auto* threads = new (Base::nothrow) RefPtr<Thread>[pair_count * 2];
    KTEST_EXPECT(threads);
    if (!threads) {
        delete[] pairs;
        return;
    }

    u64 switches_before[Processor::max_count];
    for (u32 id = 0; id < Processor::count(); ++id)
        switches_before[id] = Processor::with_id(id).context_switch_count();

    s_stop_pairs.store(false);
    for (u32 i = 0; i < pair_count; ++i) {
        threads[2 * i] = start_thread("ping"_sv, ping_thread, &pairs[i]);
        threads[2 * i + 1] = start_thread("pong"_sv, pong_thread, &pairs[i]);
    }

    u64 seconds = Tests::soak_seconds();
    u64 start = Time::now();
    for (u64 second = 1; second <= seconds; ++second) {
        Thread::sleep_until(start + second * Time::ticks_per_second());
        if (second % 10 == 0 || second == seconds) {
            u64 rounds = 0;
            for (u32 i = 0; i < pair_count; ++i)
                rounds += pairs[i].rounds.load(MemoryOrder::Relaxed);
            dmesgln("ktest:   {} s: {} round trips by {} threads", second, rounds, pair_count * 2);
        }
    }

    s_stop_pairs.store(true);
    for (u32 i = 0; i < pair_count * 2; ++i)
        threads[i]->join();

    u64 rounds = 0;
    u32 stalled_pairs = 0;
    for (u32 i = 0; i < pair_count; ++i) {
        rounds += pairs[i].rounds.load();
        if (pairs[i].rounds.load() == 0)
            ++stalled_pairs;
    }
    u32 busy_processors = 0;
    for (u32 id = 0; id < Processor::count(); ++id) {
        if (Processor::with_id(id).context_switch_count() - switches_before[id] > 100)
            ++busy_processors;
    }
    KTEST_EXPECT_EQ(stalled_pairs, 0u);
    KTEST_EXPECT_EQ(busy_processors, Processor::online_count());
    dmesgln("ktest:   {} round trips in {} s ({} per second), on {} processors", rounds, seconds, rounds / max<u64>(seconds, 1), busy_processors);

    delete[] threads;
    delete[] pairs;
}

}
