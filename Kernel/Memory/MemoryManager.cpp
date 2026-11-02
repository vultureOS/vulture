/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/MemoryLayout.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>

// Section boundaries from the linker script; each is page-aligned.
extern "C" u8 __kernel_start[];
extern "C" u8 __rodata_start[];
extern "C" u8 __data_start[];
extern "C" u8 __kernel_end[];

namespace Kernel {

using API::MemoryRegionType;
using Arch::MemoryType;
using Arch::Protection;

static unsigned order_for_bytes(size_t size)
{
    size_t pages = (size + page_size - 1) / page_size;
    unsigned order = 0;
    while ((1ull << order) < pages)
        ++order;
    return order;
}

ErrorOr<Bytes> MemoryManager::copy_to_new_pages(ReadonlyBytes data)
{
    unsigned order = order_for_bytes(data.size());
    if (order > BuddyAllocator::max_order)
        return Error::from_string_literal("boot data is too large to copy");
    auto block = TRY(physical_memory().allocate_pages(order, PageUse::BootData));
    auto* destination = physical_to_virtual(block);
    __builtin_memcpy(destination, data.data(), data.size());
    return Bytes(destination, data.size());
}

ErrorOr<void> MemoryManager::initialize(API::BootInfo const& boot_info)
{
    m_kernel_image = boot_info.kernel_image;
    m_kernel_virtual_base = boot_info.kernel_virtual_base;
    VERIFY(m_kernel_virtual_base == reinterpret_cast<FlatPtr>(__kernel_start));
    VERIFY(static_cast<u64>(__kernel_end - __kernel_start) <= m_kernel_image.size);

    // Still the loader's copy, read through the loader's direct map.
    Span<API::MemoryRegion const> loader_memory_map {
        physical_to_virtual<API::MemoryRegion const>(PhysicalAddress(boot_info.memory_map)),
        boot_info.memory_map_entries
    };
    physical_memory().initialize(loader_memory_map);

    // The memory map and device tree live in loader memory, which is
    // reclaimed later; keep copies.
    auto map_copy = TRY(copy_to_new_pages({ reinterpret_cast<u8 const*>(loader_memory_map.data()), loader_memory_map.size_in_bytes() }));
    m_memory_map = reinterpret_cast<API::MemoryRegion const*>(map_copy.data());
    m_memory_map_entries = loader_memory_map.size();
    if (boot_info.device_tree.size > 0)
        m_device_tree = TRY(copy_to_new_pages({ physical_to_virtual(PhysicalAddress(boot_info.device_tree.base)), boot_info.device_tree.size }));

    m_kernel_page_table = TRY(Arch::PageTable::create());
    TRY(map_kernel_image());
    TRY(map_physical_memory());
    m_next_mmio_address = MemoryLayout::mmio_base;
    return {};
}

ErrorOr<void> MemoryManager::map_kernel_image()
{
    struct Segment {
        u8 const* start;
        u8 const* end;
        Protection protection;
    };
    Segment const segments[] = {
        { __kernel_start, __rodata_start, Protection::ReadExecute },
        { __rodata_start, __data_start, Protection::ReadOnly },
        { __data_start, __kernel_end, Protection::ReadWrite },
    };
    for (auto const& segment : segments) {
        auto start = reinterpret_cast<FlatPtr>(segment.start);
        auto size = static_cast<size_t>(segment.end - segment.start);
        TRY(m_kernel_page_table.map(start, physical_address_of_kernel_image(segment.start), size, MemoryType::Normal, segment.protection));
    }
    return {};
}

// Every region of RAM, in runs of adjacent regions so large blocks can be
// used across region boundaries. Reserved memory and MMIO stay unmapped.
ErrorOr<void> MemoryManager::map_physical_memory()
{
    auto is_ram = [](MemoryRegionType type) {
        return type != MemoryRegionType::Reserved && type != MemoryRegionType::Mmio;
    };
    size_t index = 0;
    while (index < m_memory_map_entries) {
        auto const& first = m_memory_map[index];
        if (!is_ram(first.type)) {
            ++index;
            continue;
        }
        u64 base = first.base;
        u64 end = first.base + first.length;
        for (++index; index < m_memory_map_entries && is_ram(m_memory_map[index].type) && m_memory_map[index].base == end; ++index)
            end = m_memory_map[index].base + m_memory_map[index].length;

        base = align_down<u64>(base, page_size);
        end = align_up<u64>(end, page_size);
        if (end > MemoryLayout::direct_map_size)
            return Error::from_string_literal("RAM extends past the end of the direct map");
        TRY(m_kernel_page_table.map(MemoryLayout::direct_map_base + base, PhysicalAddress(base), end - base, MemoryType::Normal, Protection::ReadWrite));
    }
    return {};
}

void MemoryManager::activate_kernel_page_table()
{
    Arch::activate_kernel_page_table(m_kernel_page_table);
}

void MemoryManager::reclaim_boot_memory()
{
    for (auto const& region : memory_map()) {
        if (region.type != MemoryRegionType::LoaderReclaimable)
            continue;
        physical_memory().add_free_range(PhysicalAddress(region.base), region.length);
        m_reclaimed_bytes += region.length;
    }
}

ErrorOr<FlatPtr> MemoryManager::map_mmio(PhysicalAddress address, size_t size, MemoryType type)
{
    u64 base = align_down<u64>(address.get(), page_size);
    u64 offset = address.get() - base;
    u64 length = align_up<u64>(offset + size, page_size);

    SpinLockGuard guard(m_lock);
    FlatPtr virtual_address = m_next_mmio_address;
    // An unmapped page after each mapping catches overruns.
    if (virtual_address + length + page_size > MemoryLayout::mmio_base + MemoryLayout::mmio_size)
        return Error::from_string_literal("the MMIO window is full");
    TRY(m_kernel_page_table.map(virtual_address, PhysicalAddress(base), length, type, Protection::ReadWrite));
    m_next_mmio_address += length + page_size;
    return virtual_address + offset;
}

ErrorOr<KernelStack> MemoryManager::allocate_kernel_stack()
{
    size_t slot = MemoryLayout::kernel_stack_slots;
    {
        SpinLockGuard guard(m_lock);
        for (size_t i = 0; i < MemoryLayout::kernel_stack_slots; ++i) {
            if (!(m_stack_slots_in_use[i / 8] & (1 << (i % 8)))) {
                m_stack_slots_in_use[i / 8] |= static_cast<u8>(1 << (i % 8));
                slot = i;
                break;
            }
        }
    }
    if (slot == MemoryLayout::kernel_stack_slots)
        return Error::from_string_literal("out of kernel stack slots");

    KernelStack stack;
    stack.top = MemoryLayout::kernel_stacks_base + (slot + 1) * MemoryLayout::kernel_stack_slot_size;
    stack.bottom = stack.top - MemoryLayout::kernel_stack_size;
    for (FlatPtr address = stack.bottom; address < stack.top; address += page_size) {
        if (auto result = map_new_kernel_page(address); result.is_error()) {
            // Undo the pages mapped so far, and release the slot.
            free_kernel_stack({ stack.bottom, address });
            return result.release_error();
        }
    }
    Arch::finish_remapping_kernel_range(stack.bottom, stack.top - stack.bottom);
    return stack;
}

ErrorOr<void> MemoryManager::map_new_kernel_page(FlatPtr address)
{
    auto page = TRY(physical_memory().allocate_page(PageUse::KernelStack));
    SpinLockGuard guard(m_lock);
    if (auto result = m_kernel_page_table.map(address, page, page_size, MemoryType::Normal, Protection::ReadWrite); result.is_error()) {
        physical_memory().free_pages(page);
        return result.release_error();
    }
    return {};
}

void MemoryManager::free_kernel_stack(KernelStack stack)
{
    VERIFY(stack.top >= stack.bottom);
    size_t slot = (align_down<FlatPtr>(stack.bottom, MemoryLayout::kernel_stack_slot_size) - MemoryLayout::kernel_stacks_base) / MemoryLayout::kernel_stack_slot_size;
    VERIFY(slot < MemoryLayout::kernel_stack_slots);

    PhysicalAddress pages[MemoryLayout::kernel_stack_size / page_size];
    size_t page_count = 0;
    {
        SpinLockGuard guard(m_lock);
        for (FlatPtr address = stack.bottom; address < stack.top; address += page_size)
            pages[page_count++] = m_kernel_page_table.translate(address).value();
        if (stack.top > stack.bottom)
            m_kernel_page_table.unmap(stack.bottom, stack.top - stack.bottom);
        m_stack_slots_in_use[slot / 8] &= static_cast<u8>(~(1 << (slot % 8)));
    }
    // Only once no processor can reach them through the TLB.
    for (size_t i = 0; i < page_count; ++i)
        physical_memory().free_pages(pages[i]);
}

PhysicalAddress MemoryManager::physical_address_of_kernel_image(void const* pointer) const
{
    auto address = reinterpret_cast<FlatPtr>(pointer);
    VERIFY(address >= m_kernel_virtual_base && address - m_kernel_virtual_base < m_kernel_image.size);
    return PhysicalAddress(m_kernel_image.base + (address - m_kernel_virtual_base));
}

MemoryManager& memory_manager()
{
    static MemoryManager manager;
    return manager;
}

}
