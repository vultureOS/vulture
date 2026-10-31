/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>
#include <LibBase/FastRandom.h>

extern "C" void ktest_fill_vector_registers(u64 seed);
extern "C" u64 ktest_count_wrong_vector_registers(u64 seed);

namespace Kernel {

static NonnullRefPtr<Thread> spawn(StringView name, ThreadEntry entry, void* argument, ThreadOptions options = {})
{
    auto thread = Thread::create(name, entry, argument, options);
    VERIFY(!thread.is_error());
    auto started = thread.release_value();
    started->start();
    return started;
}

// A processor other than the boot thread's (0), when there is one.
static u32 spare_processor(u32 preferred)
{
    return Processor::online_count() > preferred ? preferred : 0;
}

KERNEL_TEST(threads, create_run_and_join)
{
    static Atomic<u32> s_ran;
    s_ran.store(0);
    constexpr u32 count = 200;
    RefPtr<Thread> threads[count];
    for (auto& thread : threads)
        thread = spawn("worker"_sv, [](void*) { s_ran.fetch_add(1); }, nullptr);
    for (auto& thread : threads) {
        thread->join();
        KTEST_EXPECT(thread->state() == Thread::State::Dead);
    }
    KTEST_EXPECT_EQ(s_ran.load(), count);
}

KERNEL_TEST(threads, join_after_exit_returns_at_once)
{
    auto thread = spawn("short"_sv, [](void*) { }, nullptr);
    Thread::sleep_for_microseconds(5000);
    KTEST_EXPECT(thread->state() == Thread::State::Dead);
    thread->join();
}

static void drain_all_caches()
{
    Processor::call_on_all([](void*) {
        heap_drain_processor_cache();
        physical_memory().drain_processor_cache();
    },
        nullptr);
}

// Pages that are free or sitting in slabs: creating and freeing slabs
// moves pages between the two, so their sum is what must come back.
static size_t available_pages()
{
    return physical_memory().statistics().free_pages + heap_statistics().slab_pages;
}

static void create_and_join_batch()
{
    RefPtr<Thread> threads[100];
    for (auto& thread : threads)
        thread = spawn("batch"_sv, [](void*) { Thread::yield(); }, nullptr);
    for (auto& thread : threads)
        thread->join();
}

KERNEL_TEST(threads, stacks_and_objects_are_freed)
{
    // The first batch may add page tables for the stack area, which stay.
    create_and_join_batch();
    drain_all_caches();
    size_t pages_before = available_pages();
    size_t objects_before = heap_statistics().small_objects_in_use;

    for (int round = 0; round < 10; ++round)
        create_and_join_batch();

    drain_all_caches();
    KTEST_EXPECT_EQ(available_pages(), pages_before);
    KTEST_EXPECT_EQ(heap_statistics().small_objects_in_use, objects_before);
}

KERNEL_TEST(threads, sleep_is_never_early)
{
    constexpr u32 count = 32;
    static Atomic<u32> s_early;
    static Atomic<u64> s_worst_lateness;
    s_early.store(0);
    s_worst_lateness.store(0);
    RefPtr<Thread> threads[count];
    for (u32 i = 0; i < count; ++i) {
        threads[i] = spawn("sleeper"_sv, [](void* argument) {
            FastRandom random(reinterpret_cast<FlatPtr>(argument));
            for (int round = 0; round < 5; ++round) {
                u64 deadline = Time::now() + Time::ticks_from_microseconds(random.next_between(1000, 10'000));
                Thread::sleep_until(deadline);
                u64 now = Time::now();
                if (now < deadline)
                    s_early.fetch_add(1);
                u64 lateness = now - deadline;
                u64 worst = s_worst_lateness.load();
                while (lateness > worst && !s_worst_lateness.compare_exchange_strong(worst, lateness)) {
                }
            } }, reinterpret_cast<void*>(static_cast<FlatPtr>(i + 1)));
    }
    for (auto& thread : threads)
        thread->join();
    KTEST_EXPECT_EQ(s_early.load(), 0u);
    dmesgln("ktest:   {} sleeps, latest woke {} us after its deadline", count * 5, Time::microseconds_from_ticks(s_worst_lateness.load()));
}

namespace {

constexpr u32 vector_rounds = 2000;

struct VectorRun {
    u64 seed;
    u64 wrong { 0 };
    u64 switches { 0 };
};

}

KERNEL_TEST(threads, floating_point_state_is_per_thread)
{
    // Two threads on one processor, each with its own register pattern,
    // forcing a switch between every check.
    VectorRun runs[2] = { { 0x1111'0000'0000'0000, 0, 0 }, { 0x2222'0000'0000'0000, 0, 0 } };
    u32 processor = spare_processor(1);
    RefPtr<Thread> threads[2];
    for (u32 i = 0; i < 2; ++i) {
        threads[i] = spawn("vectors"_sv, [](void* argument) {
            auto& run = *static_cast<VectorRun*>(argument);
            u64 switches_before = Thread::current().switch_count();
            ktest_fill_vector_registers(run.seed);
            for (u32 round = 0; round < vector_rounds; ++round) {
                Thread::yield();
                run.wrong += ktest_count_wrong_vector_registers(run.seed);
            }
            run.switches = Thread::current().switch_count() - switches_before;
        },
            &runs[i], { .qos = QoS::Default, .pinned_processor = processor });
    }
    for (auto& thread : threads)
        thread->join();
    for (auto const& run : runs) {
        KTEST_EXPECT_EQ(run.wrong, 0u);
        // Each yield really handed over to the other thread.
        KTEST_EXPECT(run.switches >= vector_rounds / 2);
    }
}

}
