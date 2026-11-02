/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Arch/x86_64/PageTableBuilder.h>

namespace Boot::X86_64 {

namespace {

constexpr u64 page_size = 4096;
constexpr u64 large_page_size = 2 * 1024 * 1024;

constexpr u64 present = 1ull << 0;
constexpr u64 writable = 1ull << 1;
constexpr u64 write_through = 1ull << 3;
constexpr u64 cache_disable = 1ull << 4;
constexpr u64 accessed = 1ull << 5;
constexpr u64 dirty = 1ull << 6;
constexpr u64 large = 1ull << 7;
constexpr u64 no_execute = 1ull << 63;
constexpr u64 address_mask = 0x000f'ffff'ffff'f000;

size_t table_index(u64 address, unsigned level)
{
    return (address >> (39 - 9 * level)) & 0x1ff;
}

u64 leaf_bits(MemoryAttribute attribute, Access access)
{
    u64 bits = present | accessed | dirty;
    if (attribute == MemoryAttribute::Device)
        bits |= write_through | cache_disable;
    else if (attribute == MemoryAttribute::NormalUncached)
        bits |= write_through;
    if (access == Access::ReadWrite)
        bits |= writable;
    if (access != Access::ReadExecute)
        bits |= no_execute;
    return bits;
}

}

ErrorOr<PageTableBuilder> PageTableBuilder::create(UEFI::BootServices& boot_services)
{
    PageTableBuilder builder;
    builder.m_boot_services = &boot_services;
    builder.m_root = TRY(builder.allocate_table());
    return builder;
}

ErrorOr<u64*> PageTableBuilder::allocate_table()
{
    if (m_table_page_count == m_table_pages.size())
        return Error::from_string_literal("too many page tables");
    auto const address = TRY(allocate_pages(*m_boot_services, page_size, LoaderMemoryType::LoaderReclaimable));
    m_table_pages[m_table_page_count++] = address;
    __builtin_memset(reinterpret_cast<void*>(address), 0, page_size);
    return reinterpret_cast<u64*>(address);
}

ErrorOr<u64*> PageTableBuilder::next_level(u64* table, size_t index)
{
    u64& entry = table[index];
    if (!(entry & present)) {
        auto* next = TRY(allocate_table());
        entry = reinterpret_cast<u64>(next) | present | writable;
        return next;
    }
    if (entry & large)
        return Error::from_string_literal("mapping overlaps an existing block");
    return reinterpret_cast<u64*>(entry & address_mask);
}

ErrorOr<void> PageTableBuilder::set_leaf(u64& entry, u64 value)
{
    if ((entry & present) && entry != value)
        return Error::from_string_literal("conflicting mappings for the same address");
    entry = value;
    return {};
}

ErrorOr<void> PageTableBuilder::map(u64 virtual_address, u64 physical_address, u64 size, MemoryAttribute attribute, Access access)
{
    if (virtual_address % page_size != 0 || physical_address % page_size != 0 || size % page_size != 0)
        return Error::from_string_literal("unaligned mapping");
    u64 const bits = leaf_bits(attribute, access);

    while (size > 0) {
        auto* level1 = TRY(next_level(m_root, table_index(virtual_address, 0)));
        auto* level2 = TRY(next_level(level1, table_index(virtual_address, 1)));
        bool const block_fits = virtual_address % large_page_size == 0 && physical_address % large_page_size == 0 && size >= large_page_size;
        u64 step = page_size;
        if (block_fits) {
            TRY(set_leaf(level2[table_index(virtual_address, 2)], physical_address | bits | large));
            step = large_page_size;
        } else {
            auto* level3 = TRY(next_level(level2, table_index(virtual_address, 2)));
            TRY(set_leaf(level3[table_index(virtual_address, 3)], physical_address | bits));
        }
        virtual_address += step;
        physical_address += step;
        size -= step;
    }
    return {};
}

}
