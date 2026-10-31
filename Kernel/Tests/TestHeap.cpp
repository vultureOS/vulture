/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <LibBase/FastRandom.h>

namespace Kernel {

static u8 pattern_byte(void const* pointer, size_t index)
{
    return static_cast<u8>((reinterpret_cast<FlatPtr>(pointer) >> 4) + index * 31);
}

static void fill(void* pointer, size_t size)
{
    auto* bytes = static_cast<u8*>(pointer);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = pattern_byte(pointer, i);
}

static bool is_intact(void const* pointer, size_t size)
{
    auto const* bytes = static_cast<u8 const*>(pointer);
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != pattern_byte(pointer, i))
            return false;
    }
    return true;
}

static void drain_all_heap_caches()
{
    Processor::call_on_all([](void*) { heap_drain_processor_cache(); }, nullptr);
}

KERNEL_TEST(heap, every_size_class)
{
    static constexpr size_t sizes[] = { 1, 8, 16, 17, 31, 32, 33, 64, 100, 128, 255, 256, 500, 512, 1000, 1024, 2000, 2048, 2049, 4096, 10000, 65536, 1 * MiB };
    for (size_t size : sizes) {
        void* first = kmalloc(size);
        void* second = kmalloc(size);
        KTEST_EXPECT(first && second);
        if (!first || !second)
            return;
        KTEST_EXPECT_EQ(reinterpret_cast<FlatPtr>(first) % 16, 0u);
        KTEST_EXPECT_EQ(reinterpret_cast<FlatPtr>(second) % 16, 0u);
        auto a = reinterpret_cast<FlatPtr>(first);
        auto b = reinterpret_cast<FlatPtr>(second);
        KTEST_EXPECT(a + size <= b || b + size <= a);
        fill(first, size);
        fill(second, size);
        KTEST_EXPECT(is_intact(first, size));
        KTEST_EXPECT(is_intact(second, size));
        kfree(first);
        kfree(second);
    }
    kfree(nullptr);
}

KERNEL_TEST(heap, zeroed_allocations)
{
    static constexpr size_t sizes[] = { 64, 3000 };
    for (size_t size : sizes) {
        void* dirty = kmalloc(size);
        KTEST_EXPECT(dirty);
        __builtin_memset(dirty, 0xff, size);
        kfree(dirty);
        auto* clean = static_cast<u8*>(kzalloc(size));
        KTEST_EXPECT(clean);
        bool all_zero = true;
        for (size_t i = 0; i < size; ++i)
            all_zero = all_zero && clean[i] == 0;
        KTEST_EXPECT(all_zero);
        kfree(clean);
    }
}

namespace {

struct Allocation {
    void* pointer { nullptr };
    size_t size { 0 };
};

// Allocations handed from one processor to another, to be freed there.
struct Exchange {
    static constexpr size_t capacity = 256;
    SpinLock lock;
    Allocation slots[capacity];
    size_t count { 0 };

    bool put(Allocation allocation)
    {
        SpinLockGuard guard(lock);
        if (count == capacity)
            return false;
        slots[count++] = allocation;
        return true;
    }

    Optional<Allocation> take()
    {
        SpinLockGuard guard(lock);
        if (count == 0)
            return {};
        return slots[--count];
    }
};

Exchange s_exchange;

}

static void release(Allocation allocation)
{
    KTEST_EXPECT(is_intact(allocation.pointer, allocation.size));
    kfree(allocation.pointer);
}

static void stress_heap(void*)
{
    constexpr size_t max_live = 128;
    Allocation live[max_live];
    size_t live_count = 0;
    FastRandom random(0x51ab + Processor::current().id());

    for (int step = 0; step < 30'000; ++step) {
        u64 action = random.next_below(10);
        if (action < 5 && live_count < max_live) {
            // Mostly small objects, now and then a large allocation.
            size_t size = random.next_below(16) == 0 ? random.next_between(2049, 16 * KiB) : random.next_between(1, 2048);
            void* pointer = kmalloc(size);
            KTEST_EXPECT(pointer);
            if (!pointer)
                break;
            fill(pointer, size);
            live[live_count++] = { pointer, size };
        } else if (action < 8 && live_count > 0) {
            size_t index = random.next_below(live_count);
            release(live[index]);
            live[index] = live[--live_count];
        } else if (action < 9 && live_count > 0) {
            size_t index = random.next_below(live_count);
            if (s_exchange.put(live[index]))
                live[index] = live[--live_count];
        } else if (auto handed = s_exchange.take(); handed.has_value()) {
            release(handed.value());
        }
    }
    while (live_count > 0)
        release(live[--live_count]);
}

KERNEL_TEST(heap, concurrent_stress_with_cross_processor_frees)
{
    drain_all_heap_caches();
    auto before = heap_statistics();
    Processor::run_on_all(stress_heap, nullptr);
    for (auto handed = s_exchange.take(); handed.has_value(); handed = s_exchange.take())
        release(handed.value());
    drain_all_heap_caches();
    auto after = heap_statistics();
    KTEST_EXPECT_EQ(after.small_objects_in_use, before.small_objects_in_use);
    KTEST_EXPECT_EQ(after.large_pages, before.large_pages);
    dmesgln("ktest:   slabs hold {} KiB; {} small objects in use", after.slab_pages * page_size / KiB, after.small_objects_in_use);
}

}
