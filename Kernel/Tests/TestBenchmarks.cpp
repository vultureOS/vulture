/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/Mutex.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>

// Measurements rather than checks; "ktest=bench" runs just these. Numbers
// are for QEMU with Apple's hypervisor, where waking an idle processor
// costs a trip through the host.

namespace Kernel {

static NonnullRefPtr<Thread> start_thread(ThreadEntry entry, void* argument, ThreadOptions options = {})
{
    auto thread = Thread::create("bench"_sv, entry, argument, options);
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

constexpr u32 rounds = 20'000;

struct YieldRun {
    Atomic<u32> ready { 0 };
    u64 start { 0 };
    u64 end { 0 };
};

struct PingPong {
    Semaphore ping { 0 };
    Semaphore pong { 0 };
    u64 start { 0 };
    u64 end { 0 };
};

}

KERNEL_TEST(bench, context_switch_by_yield)
{
    // Two threads on one processor yield to each other: every yield is
    // one switch.
    YieldRun run;
    u32 processor = spare_processor(1);
    auto body = [](void* argument) {
        auto& state = *static_cast<YieldRun*>(argument);
        state.ready.fetch_add(1);
        while (state.ready.load() < 2)
            Thread::yield();
        if (!state.start)
            state.start = Time::now();
        for (u32 i = 0; i < rounds; ++i)
            Thread::yield();
        state.end = Time::now();
    };
    auto first = start_thread(body, &run, { .qos = QoS::Default, .pinned_processor = processor });
    auto second = start_thread(body, &run, { .qos = QoS::Default, .pinned_processor = processor });
    first->join();
    second->join();
    u64 nanoseconds = Time::nanoseconds_from_ticks(run.end - run.start) / (2 * rounds);
    dmesgln("ktest:   bench: context switch (yield, one processor): {} ns", nanoseconds);
}

static void run_ping_pong(PingPong& state, u32 ping_processor, u32 pong_processor)
{
    auto ping = start_thread([](void* argument) {
        auto& run = *static_cast<PingPong*>(argument);
        run.start = Time::now();
        for (u32 i = 0; i < rounds; ++i) {
            run.pong.up();
            run.ping.down();
        }
        run.end = Time::now();
    },
        &state, { .qos = QoS::Default, .pinned_processor = ping_processor });
    auto pong = start_thread([](void* argument) {
        auto& run = *static_cast<PingPong*>(argument);
        for (u32 i = 0; i < rounds; ++i) {
            run.pong.down();
            run.ping.up();
        }
    },
        &state, { .qos = QoS::Default, .pinned_processor = pong_processor });
    ping->join();
    pong->join();
}

KERNEL_TEST(bench, semaphore_round_trip_one_processor)
{
    PingPong state;
    u32 processor = spare_processor(1);
    run_ping_pong(state, processor, processor);
    u64 nanoseconds = Time::nanoseconds_from_ticks(state.end - state.start) / rounds;
    dmesgln("ktest:   bench: semaphore round trip (two wakeups, two switches), one processor: {} ns", nanoseconds);
}

KERNEL_TEST(bench, semaphore_round_trip_two_processors)
{
    PingPong state;
    run_ping_pong(state, spare_processor(1), spare_processor(2));
    u64 nanoseconds = Time::nanoseconds_from_ticks(state.end - state.start) / rounds;
    dmesgln("ktest:   bench: semaphore round trip, two processors: {} ns", nanoseconds);
}

KERNEL_TEST(bench, thread_create_start_join)
{
    constexpr u32 count = 1000;
    u64 start = Time::now();
    for (u32 i = 0; i < count; ++i)
        start_thread([](void*) { }, nullptr)->join();
    u64 nanoseconds = Time::nanoseconds_from_ticks(Time::now() - start) / count;
    dmesgln("ktest:   bench: create, start and join a thread: {} ns", nanoseconds);
}

}
