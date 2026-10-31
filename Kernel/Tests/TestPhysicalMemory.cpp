/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>
#include <LibBase/FastRandom.h>

namespace Kernel {

// Puts every cached page back into the zones, so free counts are exact.
// In interrupt context on each processor: threads would churn the caches.
static void drain_all_page_caches()
{
    Processor::call_on_all([](void*) { physical_memory().drain_processor_cache(); }, nullptr);
}

KERNEL_TEST(physical_memory, every_order_is_aligned_and_usable)
{
    drain_all_page_caches();
    auto before = physical_memory().statistics();

    PhysicalAddress blocks[BuddyAllocator::max_order + 1];
    for (unsigned order = 0; order <= BuddyAllocator::max_order; ++order) {
        auto block = physical_memory().allocate_pages(order, PageUse::General);
        KTEST_EXPECT(!block.is_error());
        if (block.is_error())
            return;
        blocks[order] = block.value();
        u64 size = page_size << order;
        KTEST_EXPECT_EQ(blocks[order].get() % size, 0u);
        KTEST_EXPECT(physical_memory().block_use(blocks[order]) == PageUse::General);
        auto* bytes = physical_to_virtual(blocks[order]);
        bytes[0] = 0x5a;
        bytes[size - 1] = 0xa5;
        KTEST_EXPECT(bytes[0] == 0x5a && bytes[size - 1] == 0xa5);
    }
    for (auto block : blocks)
        physical_memory().free_pages(block);

    physical_memory().drain_processor_cache();
    auto after = physical_memory().statistics();
    KTEST_EXPECT_EQ(after.free_pages, before.free_pages);
    KTEST_EXPECT_EQ(after.largest_free_blocks, before.largest_free_blocks);
}

KERNEL_TEST(physical_memory, exhaust_and_recover)
{
    // Pages taken are recorded in a bitmap rather than written to: under a
    // hypervisor, touching every page would make the host commit all of
    // the guest's memory.
    u64 lowest = ~0ull;
    u64 highest = 0;
    physical_memory().for_each_zone([&](BuddyAllocator const& zone) {
        lowest = min(lowest, zone.base().get());
        highest = max(highest, zone.base().get() + zone.page_count() * page_size);
    });
    size_t frames = (highest - lowest) / page_size;
    auto* taken = static_cast<u8*>(kzalloc((frames + 7) / 8));
    KTEST_EXPECT(taken);
    if (!taken)
        return;

    drain_all_page_caches();
    auto before = physical_memory().statistics();
    u64 start = Time::now();

    size_t count = 0;
    for (;;) {
        auto page = physical_memory().allocate_page(PageUse::General);
        if (page.is_error())
            break;
        size_t frame = (page.value().get() - lowest) / page_size;
        KTEST_EXPECT(!(taken[frame / 8] & (1 << (frame % 8))));
        taken[frame / 8] |= static_cast<u8>(1 << (frame % 8));
        ++count;
    }
    KTEST_EXPECT_EQ(count, before.free_pages);
    KTEST_EXPECT(physical_memory().allocate_pages(3, PageUse::General).is_error());
    u64 taken_at = Time::now();

    for (size_t frame = 0; frame < frames; ++frame) {
        if (taken[frame / 8] & (1 << (frame % 8)))
            physical_memory().free_pages(PhysicalAddress(lowest + frame * page_size));
    }
    physical_memory().drain_processor_cache();
    u64 returned_at = Time::now();

    // Freed in address order, page by page: merging must rebuild every large block.
    auto after = physical_memory().statistics();
    kfree(taken);
    KTEST_EXPECT_EQ(after.free_pages, before.free_pages);
    KTEST_EXPECT_EQ(after.largest_free_blocks, before.largest_free_blocks);
    dmesgln("ktest:   {} pages ({} MiB) taken in {} ms, returned in {} ms", count, count * page_size / MiB,
        Time::microseconds_from_ticks(taken_at - start) / 1000, Time::microseconds_from_ticks(returned_at - taken_at) / 1000);
}

static void stress_physical_memory(void*)
{
    struct Block {
        PhysicalAddress address;
        unsigned order;
        u64 stamp;
    };
    constexpr size_t max_live = 64;
    Block live[max_live];
    size_t live_count = 0;
    u32 processor = Processor::current().id();
    FastRandom random(0x9e3779b9 + processor);

    auto stamp_of = [&](u64 iteration) { return (u64(processor) << 48) | iteration; };
    auto release = [&](Block const& block) {
        auto* words = physical_to_virtual<u64>(block.address);
        size_t last = ((page_size << block.order) / sizeof(u64)) - 1;
        KTEST_EXPECT(words[0] == block.stamp && words[last] == ~block.stamp);
        physical_memory().free_pages(block.address);
    };

    for (u64 iteration = 0; iteration < 20'000; ++iteration) {
        bool allocate = live_count == 0 || (live_count < max_live && random.next_below(2) == 0);
        if (!allocate) {
            size_t index = random.next_below(live_count);
            release(live[index]);
            live[index] = live[--live_count];
            continue;
        }
        // Mostly single pages (the per-processor cache path), some blocks.
        unsigned order = random.next_below(4) == 0 ? static_cast<unsigned>(random.next_between(1, 4)) : 0;
        auto block = physical_memory().allocate_pages(order, PageUse::General);
        KTEST_EXPECT(!block.is_error());
        if (block.is_error())
            break;
        auto* words = physical_to_virtual<u64>(block.value());
        u64 stamp = stamp_of(iteration);
        words[0] = stamp;
        words[((page_size << order) / sizeof(u64)) - 1] = ~stamp;
        live[live_count++] = { block.value(), order, stamp };
    }
    while (live_count > 0)
        release(live[--live_count]);
}

KERNEL_TEST(physical_memory, concurrent_stress)
{
    drain_all_page_caches();
    auto before = physical_memory().statistics();
    Processor::run_on_all(stress_physical_memory, nullptr);
    drain_all_page_caches();
    auto after = physical_memory().statistics();
    KTEST_EXPECT_EQ(after.free_pages, before.free_pages);
}

}
