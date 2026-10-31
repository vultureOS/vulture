/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Memory/BuddyAllocator.h>
#include <LibBase/FastRandom.h>
#include <LibTest/TestCase.h>

using Kernel::BuddyAllocator;
using Kernel::page_size;
using Kernel::PhysicalAddress;

namespace {

// 8 MiB of "physical memory" at a made-up address with the alignment a
// real machine would give it. Accesses go to `s_memory`.
constexpr u64 fake_base = 0x4000'0000;
constexpr size_t memory_pages = 2048;
alignas(16) u8 s_memory[memory_pages * page_size];
alignas(16) u8 s_metadata[BuddyAllocator::metadata_size(memory_pages)];

FlatPtr access_offset()
{
    return reinterpret_cast<FlatPtr>(s_memory) - fake_base;
}

void initialize(BuddyAllocator& allocator)
{
    allocator.initialize(PhysicalAddress(fake_base), memory_pages, Bytes(s_metadata, sizeof(s_metadata)), access_offset());
}

u8* memory_at(PhysicalAddress address)
{
    return reinterpret_cast<u8*>(address.get() + access_offset());
}

}

TEST_CASE(buddy_whole_range_forms_largest_blocks)
{
    BuddyAllocator allocator;
    initialize(allocator);
    EXPECT_EQ(allocator.free_pages(), 0u);
    allocator.add_free_range(PhysicalAddress(fake_base), memory_pages);
    EXPECT_EQ(allocator.free_pages(), memory_pages);
    EXPECT_EQ(allocator.free_blocks(BuddyAllocator::max_order), 2u);
    for (unsigned order = 0; order < BuddyAllocator::max_order; ++order)
        EXPECT_EQ(allocator.free_blocks(order), 0u);
}

TEST_CASE(buddy_split_and_merge_back)
{
    BuddyAllocator allocator;
    initialize(allocator);
    allocator.add_free_range(PhysicalAddress(fake_base), memory_pages);

    auto page = allocator.allocate(0, 5);
    EXPECT(page.has_value());
    EXPECT_EQ(allocator.free_pages(), memory_pages - 1);
    // Splitting one 4 MiB block leaves one free block of every smaller order.
    for (unsigned order = 0; order < BuddyAllocator::max_order; ++order)
        EXPECT_EQ(allocator.free_blocks(order), 1u);
    EXPECT_EQ(allocator.free_blocks(BuddyAllocator::max_order), 1u);
    EXPECT_EQ(allocator.block_tag(page.value()).value(), 5);
    EXPECT_EQ(allocator.block_order(page.value()).value(), 0u);

    EXPECT_EQ(allocator.free(page.value()), 0u);
    EXPECT_EQ(allocator.free_pages(), memory_pages);
    EXPECT_EQ(allocator.free_blocks(BuddyAllocator::max_order), 2u);
    EXPECT(!allocator.block_tag(page.value()).has_value());
}

TEST_CASE(buddy_blocks_are_aligned_to_their_size)
{
    BuddyAllocator allocator;
    initialize(allocator);
    allocator.add_free_range(PhysicalAddress(fake_base), memory_pages);

    Optional<PhysicalAddress> blocks[BuddyAllocator::max_order + 1];
    for (unsigned order = 0; order <= BuddyAllocator::max_order; ++order) {
        blocks[order] = allocator.allocate(order, 1);
        EXPECT(blocks[order].has_value());
        u64 size = page_size << order;
        EXPECT_EQ(blocks[order]->get() % size, 0u);
        // Touch both ends, so the address sanitizer would catch a block
        // reaching past the memory.
        memory_at(blocks[order].value())[0] = 1;
        memory_at(blocks[order].value())[size - 1] = 1;
        // A pointer into the middle of a block is not a block.
        if (order > 0)
            EXPECT(!allocator.block_order(blocks[order]->offset(page_size)).has_value());
    }
    for (auto& block : blocks)
        allocator.free(block.value());
    EXPECT_EQ(allocator.free_pages(), memory_pages);
    EXPECT_EQ(allocator.free_blocks(BuddyAllocator::max_order), 2u);
}

TEST_CASE(buddy_unaligned_range_exhaust_and_recover)
{
    BuddyAllocator allocator;
    initialize(allocator);
    // An odd range: starts 3 pages in and covers 1000 pages.
    auto start = PhysicalAddress(fake_base + 3 * page_size);
    constexpr size_t count = 1000;
    allocator.add_free_range(start, count);
    EXPECT_EQ(allocator.free_pages(), count);

    size_t initial_blocks[BuddyAllocator::max_order + 1];
    for (unsigned order = 0; order <= BuddyAllocator::max_order; ++order)
        initial_blocks[order] = allocator.free_blocks(order);

    static PhysicalAddress pages[count];
    bool seen[memory_pages] {};
    for (size_t i = 0; i < count; ++i) {
        auto page = allocator.allocate(0, 0);
        EXPECT(page.has_value());
        EXPECT(start <= page.value() && page.value() < start.offset(count * page_size));
        size_t index = (page->get() - fake_base) / page_size;
        EXPECT(!seen[index]);
        seen[index] = true;
        pages[i] = page.value();
    }
    EXPECT(!allocator.allocate(0, 0).has_value());
    EXPECT_EQ(allocator.free_pages(), 0u);

    // Free in a scrambled order; merging must still rebuild every block.
    FastRandom random(1);
    for (size_t i = count - 1; i > 0; --i)
        swap(pages[i], pages[random.next_below(i + 1)]);
    for (auto page : pages)
        allocator.free(page);
    EXPECT_EQ(allocator.free_pages(), count);
    for (unsigned order = 0; order <= BuddyAllocator::max_order; ++order)
        EXPECT_EQ(allocator.free_blocks(order), initial_blocks[order]);
}

TEST_CASE(buddy_random_stress_never_overlaps)
{
    BuddyAllocator allocator;
    initialize(allocator);
    allocator.add_free_range(PhysicalAddress(fake_base), memory_pages);

    struct Live {
        PhysicalAddress address;
        unsigned order;
    };
    static Live live[512];
    size_t live_count = 0;
    static u8 owner[memory_pages];
    __builtin_memset(owner, 0, sizeof(owner));

    FastRandom random(99);
    for (int step = 0; step < 20000; ++step) {
        bool allocate = live_count == 0 || (live_count < 512 && random.next_below(2) == 0);
        if (allocate) {
            unsigned order = static_cast<unsigned>(random.next_below(6));
            auto block = allocator.allocate(order, 0);
            if (!block.has_value())
                continue;
            size_t first = (block->get() - fake_base) / page_size;
            for (size_t page = first; page < first + (1u << order); ++page) {
                EXPECT_EQ(owner[page], 0);
                owner[page] = 1;
            }
            live[live_count++] = { block.value(), order };
        } else {
            size_t index = random.next_below(live_count);
            auto victim = live[index];
            live[index] = live[--live_count];
            size_t first = (victim.address.get() - fake_base) / page_size;
            for (size_t page = first; page < first + (1u << victim.order); ++page)
                owner[page] = 0;
            EXPECT_EQ(allocator.free(victim.address), victim.order);
        }
    }
    while (live_count > 0)
        allocator.free(live[--live_count].address);
    EXPECT_EQ(allocator.free_pages(), memory_pages);
    EXPECT_EQ(allocator.free_blocks(BuddyAllocator::max_order), 2u);
}
