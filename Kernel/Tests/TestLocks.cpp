/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>

namespace Kernel {

static NonnullRefPtr<Thread> start_thread(ThreadEntry entry, void* argument, ThreadOptions options = {})
{
    auto thread = Thread::create("lock-test"_sv, entry, argument, options);
    VERIFY(!thread.is_error());
    auto started = thread.release_value();
    started->start();
    return started;
}

namespace {

struct MutexRun {
    Mutex mutex;
    u64 counter { 0 };
    Atomic<u32> inside { 0 };
    Atomic<u32> overlaps { 0 };
};

constexpr u32 mutex_threads = 32;
constexpr u32 mutex_iterations = 2000;

}

KERNEL_TEST(locks, mutex_mutual_exclusion)
{
    MutexRun run;
    RefPtr<Thread> threads[mutex_threads];
    for (auto& thread : threads) {
        thread = start_thread([](void* argument) {
            auto& state = *static_cast<MutexRun*>(argument);
            for (u32 i = 0; i < mutex_iterations; ++i) {
                MutexLocker locker(state.mutex);
                if (state.inside.fetch_add(1) != 0)
                    state.overlaps.fetch_add(1);
                state.counter = state.counter + 1;
                // Now and then, hold the lock across a switch, so others
                // queue up and the hand-over path runs.
                if (i % 64 == 0)
                    Thread::yield();
                state.inside.fetch_sub(1);
            }
        },
            &run);
    }
    for (auto& thread : threads)
        thread->join();
    KTEST_EXPECT_EQ(run.counter, u64(mutex_threads) * mutex_iterations);
    KTEST_EXPECT_EQ(run.overlaps.load(), 0u);
}

KERNEL_TEST(locks, mutex_try_lock_and_ownership)
{
    Mutex mutex;
    KTEST_EXPECT(mutex.try_lock());
    KTEST_EXPECT(mutex.is_locked_by_current_thread());
    struct Probe {
        Mutex* mutex;
        bool got_it { true };
    } probe { &mutex };
    start_thread([](void* argument) {
        auto& p = *static_cast<Probe*>(argument);
        p.got_it = p.mutex->try_lock();
    },
        &probe)
        ->join();
    KTEST_EXPECT(!probe.got_it);
    mutex.unlock();
    KTEST_EXPECT(!mutex.is_locked_by_current_thread());
}

namespace {

struct SemaphoreRun {
    Semaphore slots { 3 };
    Atomic<u32> inside { 0 };
    Atomic<u32> most_inside { 0 };
};

}

KERNEL_TEST(locks, semaphore_limits_concurrency)
{
    SemaphoreRun run;
    RefPtr<Thread> threads[16];
    for (auto& thread : threads) {
        thread = start_thread([](void* argument) {
            auto& state = *static_cast<SemaphoreRun*>(argument);
            for (int round = 0; round < 10; ++round) {
                state.slots.down();
                u32 now_inside = state.inside.fetch_add(1) + 1;
                u32 most = state.most_inside.load();
                while (now_inside > most && !state.most_inside.compare_exchange_strong(most, now_inside)) {
                }
                Thread::sleep_for_microseconds(200);
                state.inside.fetch_sub(1);
                state.slots.up();
            }
        },
            &run);
    }
    for (auto& thread : threads)
        thread->join();
    KTEST_EXPECT(run.most_inside.load() <= 3);
    KTEST_EXPECT(run.most_inside.load() >= 2);
    KTEST_EXPECT(run.slots.try_down() && run.slots.try_down() && run.slots.try_down());
    KTEST_EXPECT(!run.slots.try_down());
}

namespace {

struct WaitRun {
    WaitQueue queue;
    Atomic<bool> go { false };
    Atomic<u32> passed { 0 };
};

}

KERNEL_TEST(locks, wait_queue_wakes_every_waiter)
{
    WaitRun run;
    RefPtr<Thread> threads[16];
    for (auto& thread : threads) {
        thread = start_thread([](void* argument) {
            auto& state = *static_cast<WaitRun*>(argument);
            state.queue.wait_until([&] { return state.go.load(); });
            state.passed.fetch_add(1);
        },
            &run);
    }
    Thread::sleep_for_microseconds(10'000);
    KTEST_EXPECT_EQ(run.passed.load(), 0u);
    run.go.store(true);
    run.queue.wake_all();
    for (auto& thread : threads)
        thread->join();
    KTEST_EXPECT_EQ(run.passed.load(), 16u);
}

}
