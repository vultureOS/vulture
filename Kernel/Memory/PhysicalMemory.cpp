/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>

namespace Kernel {

using API::MemoryRegionType;

// Memory map types that are RAM. Reserved is left out: it may be RAM, but
// it may also be something that must never be touched.
static bool is_ram(MemoryRegionType type)
{
    return type != MemoryRegionType::Reserved && type != MemoryRegionType::Mmio;
}

static u8* direct_map_pointer(PhysicalAddress address)
{
    return reinterpret_cast<u8*>(API::direct_map_base + address.get());
}

namespace {

struct Range {
    u64 base;
    u64 end;
};

// Usable memory before the zones exist. The zones' own bookkeeping is
// carved out of it first.
struct UsableRanges {
    static constexpr size_t capacity = 64;
    Range ranges[capacity];
    size_t count { 0 };

    void add(u64 base, u64 end)
    {
        base = align_up<u64>(base, page_size);
        end = align_down<u64>(end, page_size);
        // Page 0 stays unused, so a physical address of 0 is never valid.
        if (base == 0)
            base = page_size;
        if (base >= end)
            return;
        if (count == capacity) {
            dmesgln("Physical memory: too many usable regions, ignoring {:#x}-{:#x}", base, end - 1);
            return;
        }
        ranges[count++] = { base, end };
    }

    // Takes `size` bytes from the end of the largest range.
    Optional<u64> take(u64 size)
    {
        Range* largest = nullptr;
        for (size_t i = 0; i < count; ++i) {
            if (!largest || ranges[i].end - ranges[i].base > largest->end - largest->base)
                largest = &ranges[i];
        }
        if (!largest || largest->end - largest->base < size)
            return {};
        largest->end -= size;
        return largest->end;
    }
};

}

void PhysicalMemoryManager::initialize(Span<API::MemoryRegion const> memory_map)
{
    // Zones: maximal runs of address-contiguous RAM.
    Range zones[max_zones];
    size_t zone_count = 0;
    UsableRanges usable;
    for (auto const& region : memory_map) {
        if (!is_ram(region.type))
            continue;
        u64 end = region.base + region.length;
        if (zone_count > 0 && zones[zone_count - 1].end == region.base) {
            zones[zone_count - 1].end = end;
        } else if (zone_count < max_zones) {
            zones[zone_count++] = { region.base, end };
        } else {
            dmesgln("Physical memory: too many zones, ignoring {:#x}-{:#x}", region.base, end - 1);
            continue;
        }
        if (region.type == MemoryRegionType::Usable)
            usable.add(region.base, end);
    }

    for (size_t i = 0; i < zone_count; ++i) {
        u64 base = align_up<u64>(zones[i].base, page_size);
        u64 end = align_down<u64>(zones[i].end, page_size);
        if (base >= end)
            continue;
        size_t pages = (end - base) / page_size;
        u64 metadata_size = align_up<u64>(BuddyAllocator::metadata_size(pages), page_size);
        auto metadata = usable.take(metadata_size);
        if (!metadata.has_value()) {
            dmesgln("Physical memory: no room for the bookkeeping of zone {:#x}-{:#x}", base, end - 1);
            continue;
        }
        auto& zone = m_zones[m_zone_count++];
        zone.initialize(PhysicalAddress(base), pages, Bytes(direct_map_pointer(PhysicalAddress(metadata.value())), metadata_size), API::direct_map_base);
    }

    for (size_t i = 0; i < usable.count; ++i) {
        auto const& range = usable.ranges[i];
        if (range.end > range.base)
            add_free_range(PhysicalAddress(range.base), range.end - range.base);
    }
}

BuddyAllocator* PhysicalMemoryManager::zone_containing(PhysicalAddress address)
{
    for (size_t i = 0; i < m_zone_count; ++i) {
        if (m_zones[i].contains(address))
            return &m_zones[i];
    }
    return nullptr;
}

BuddyAllocator const* PhysicalMemoryManager::zone_containing(PhysicalAddress address) const
{
    return const_cast<PhysicalMemoryManager*>(this)->zone_containing(address);
}

void PhysicalMemoryManager::add_free_range(PhysicalAddress base, u64 size)
{
    u64 start = align_up<u64>(base.get(), page_size);
    u64 end = align_down<u64>(base.get() + size, page_size);
    if (start == 0)
        start = page_size;
    if (start >= end)
        return;
    SpinLockGuard guard(m_lock);
    auto* zone = zone_containing(PhysicalAddress(start));
    VERIFY(zone && zone->contains(PhysicalAddress(end - page_size)));
    zone->add_free_range(PhysicalAddress(start), (end - start) / page_size);
}

Optional<PhysicalAddress> PhysicalMemoryManager::allocate_from_zones(unsigned order, PageUse use)
{
    VERIFY(m_lock.is_locked_by_current_processor());
    for (size_t i = 0; i < m_zone_count; ++i) {
        if (auto block = m_zones[i].allocate(order, to_underlying(use)); block.has_value())
            return block;
    }
    return {};
}

void PhysicalMemoryManager::refill_cache(PhysicalPageCache& cache)
{
    SpinLockGuard guard(m_lock);
    while (cache.count < PhysicalPageCache::batch) {
        auto page = allocate_from_zones(0, PageUse::Cached);
        if (!page.has_value())
            break;
        cache.pages[cache.count++] = page.value();
    }
}

void PhysicalMemoryManager::flush_cache(PhysicalPageCache& cache, u32 count)
{
    SpinLockGuard guard(m_lock);
    for (u32 i = 0; i < count && cache.count > 0; ++i) {
        auto page = cache.pages[--cache.count];
        zone_containing(page)->free(page);
    }
}

ErrorOr<PhysicalAddress> PhysicalMemoryManager::allocate_pages(unsigned order, PageUse use)
{
    VERIFY(order <= BuddyAllocator::max_order);
    if (order == 0) {
        InterruptDisabler disabler;
        auto& cache = Processor::current().page_cache();
        if (cache.count == 0)
            refill_cache(cache);
        if (cache.count == 0)
            return Error::from_string_literal("out of physical memory");
        auto page = cache.pages[--cache.count];
        // The page is this processor's alone now, so retagging needs no lock.
        zone_containing(page)->set_block_tag(page, to_underlying(use));
        return page;
    }

    SpinLockGuard guard(m_lock);
    auto block = allocate_from_zones(order, use);
    if (!block.has_value())
        return Error::from_string_literal("out of physical memory");
    return block.value();
}

ErrorOr<PhysicalAddress> PhysicalMemoryManager::allocate_zeroed_page(PageUse use)
{
    auto page = TRY(allocate_page(use));
    __builtin_memset(direct_map_pointer(page), 0, page_size);
    return page;
}

unsigned PhysicalMemoryManager::free_pages(PhysicalAddress address)
{
    auto* zone = zone_containing(address);
    VERIFY(zone);
    // Reading the order of a block we own needs no lock.
    auto order = zone->block_order(address);
    VERIFY(order.has_value());
    VERIFY(zone->block_tag(address).value() != to_underlying(PageUse::Cached));

    if (order.value() == 0) {
        InterruptDisabler disabler;
        auto& cache = Processor::current().page_cache();
        if (cache.count == PhysicalPageCache::capacity)
            flush_cache(cache, PhysicalPageCache::batch);
        zone->set_block_tag(address, to_underlying(PageUse::Cached));
        cache.pages[cache.count++] = address;
        return 0;
    }

    SpinLockGuard guard(m_lock);
    return zone->free(address);
}

PageUse PhysicalMemoryManager::block_use(PhysicalAddress address) const
{
    auto const* zone = zone_containing(address);
    if (!zone)
        return PageUse::Unknown;
    auto tag = zone->block_tag(address);
    return tag.has_value() ? static_cast<PageUse>(tag.value()) : PageUse::Unknown;
}

void PhysicalMemoryManager::drain_processor_cache()
{
    InterruptDisabler disabler;
    auto& cache = Processor::current().page_cache();
    flush_cache(cache, cache.count);
}

PhysicalMemoryStatistics PhysicalMemoryManager::statistics() const
{
    SpinLockGuard guard(m_lock);
    PhysicalMemoryStatistics statistics;
    statistics.zone_count = m_zone_count;
    for (size_t i = 0; i < m_zone_count; ++i) {
        statistics.managed_pages += m_zones[i].page_count();
        statistics.free_pages += m_zones[i].free_pages();
        statistics.largest_free_blocks += m_zones[i].free_blocks(BuddyAllocator::max_order);
    }
    return statistics;
}

PhysicalMemoryManager& physical_memory()
{
    static PhysicalMemoryManager manager;
    return manager;
}

}
