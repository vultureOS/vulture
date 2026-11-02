/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/BootInfo.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Memory/BuddyAllocator.h>
#include <LibBase/Atomic.h>
#include <LibBase/Error.h>

namespace Kernel {

// What an allocated block is for: recorded with the block, to check frees
// and to account for memory.
enum class PageUse : u8 {
    Unknown = 0,
    General,
    PageTable,
    Slab,
    HeapLarge,
    KernelStack,
    BootData,
    // Mapped into user space; may be shared copy-on-write (counted).
    User,
    // Sitting in a processor's page cache, not handed out.
    Cached,
};

// Single pages a processor freed recently, reused without taking the global
// lock. One per processor (see Processor::page_cache()).
struct PhysicalPageCache {
    static constexpr u32 capacity = 64;
    static constexpr u32 batch = 32; // Pages moved per refill or flush.

    u32 count { 0 };
    PhysicalAddress pages[capacity];
};

struct PhysicalMemoryStatistics {
    size_t managed_pages { 0 }; // In any zone, including reserved ones.
    size_t free_pages { 0 };
    // Free blocks of the largest order: a measure of fragmentation.
    size_t largest_free_blocks { 0 };
    size_t zone_count { 0 };
};

// All physical RAM, as buddy-allocated zones. A zone is a contiguous run of
// RAM in the memory map; only regions the loader marked usable (and later
// reclaimed loader memory) are ever handed out.
class PhysicalMemoryManager {
public:
    static constexpr size_t max_zones = 16;

    // Reads the memory map through the direct map.
    void initialize(Span<API::MemoryRegion const>);

    // Makes more memory available, such as the loader's once the kernel no
    // longer needs it. Must lie inside a zone.
    void add_free_range(PhysicalAddress, u64 size);

    [[nodiscard]] ErrorOr<PhysicalAddress> allocate_pages(unsigned order, PageUse);
    [[nodiscard]] ErrorOr<PhysicalAddress> allocate_page(PageUse use) { return allocate_pages(0, use); }
    // A page filled with zeros.
    [[nodiscard]] ErrorOr<PhysicalAddress> allocate_zeroed_page(PageUse);
    // Returns the order of the freed block.
    unsigned free_pages(PhysicalAddress);

    // The use of the allocated block that starts at `address`, or Unknown.
    [[nodiscard]] PageUse block_use(PhysicalAddress) const;

    // User pages carry a count of the address spaces sharing them (after
    // fork, copy-on-write). A new one is zeroed, with a count of 1; the
    // last unref frees it.
    [[nodiscard]] ErrorOr<PhysicalAddress> allocate_user_page();
    void ref_user_page(PhysicalAddress);
    void unref_user_page(PhysicalAddress);
    [[nodiscard]] u32 user_page_references(PhysicalAddress) const;

    // Returns this processor's cached pages to the zones.
    void drain_processor_cache();

    // Free pages in the zones; pages in processor caches are not counted.
    [[nodiscard]] PhysicalMemoryStatistics statistics() const;

    // How free memory is doing, for the compressor (Memory/Compressor.h), from how much is managed.
    void set_pressure_levels(size_t managed_pages);
    // Time to make some room.
    [[nodiscard]] bool is_low() const { return m_free_estimate.load() < m_low_pages; }
    // Enough room has been made.
    [[nodiscard]] bool has_plenty_free() const { return m_free_estimate.load() >= m_plenty_pages; }
    // What is left is for the kernel, and for making room: not for programs.
    [[nodiscard]] bool is_nearly_out() const { return m_free_estimate.load() < m_reserved_pages; }

    template<typename Callback>
    void for_each_zone(Callback callback) const
    {
        for (size_t i = 0; i < m_zone_count; ++i)
            callback(m_zones[i]);
    }

private:
    [[nodiscard]] BuddyAllocator* zone_containing(PhysicalAddress);
    [[nodiscard]] BuddyAllocator const* zone_containing(PhysicalAddress) const;
    [[nodiscard]] u16* reference_count(PhysicalAddress) const;
    [[nodiscard]] Optional<PhysicalAddress> allocate_from_zones(unsigned order, PageUse);
    void refill_cache(PhysicalPageCache&);
    void flush_cache(PhysicalPageCache&, u32 count);
    void update_free_estimate();

    mutable SpinLock m_lock;
    BuddyAllocator m_zones[max_zones];
    u16* m_reference_counts[max_zones] {};
    size_t m_zone_count { 0 };
    // In the zones, as of the last time they changed.
    Atomic<size_t> m_free_estimate { 0 };
    size_t m_low_pages { 0 };
    size_t m_plenty_pages { 0 };
    size_t m_reserved_pages { 0 };
};

PhysicalMemoryManager& physical_memory();

}
