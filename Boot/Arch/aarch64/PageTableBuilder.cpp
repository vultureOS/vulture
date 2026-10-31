/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Arch/aarch64/PageTableBuilder.h>

namespace Boot::AArch64 {

using namespace Kernel::AArch64;

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
    auto address = TRY(allocate_pages(*m_boot_services, page_size, LoaderMemoryType::LoaderReclaimable));
    m_table_pages[m_table_page_count++] = address;
    return reinterpret_cast<u64*>(address);
}

ErrorOr<u64*> PageTableBuilder::next_level(u64* table, size_t index)
{
    u64& entry = table[index];
    if (!(entry & Descriptor::valid)) {
        auto* next = TRY(allocate_table());
        entry = table_descriptor(reinterpret_cast<u64>(next));
        return next;
    }
    if (!(entry & Descriptor::table))
        return Error::from_string_literal("mapping overlaps an existing block");
    return reinterpret_cast<u64*>(entry & Descriptor::address_mask);
}

ErrorOr<void> PageTableBuilder::set_leaf(u64& entry, u64 value)
{
    if ((entry & Descriptor::valid) && entry != value)
        return Error::from_string_literal("conflicting mappings for the same address");
    entry = value;
    return {};
}

ErrorOr<void> PageTableBuilder::map(u64 virtual_address, u64 physical_address, u64 size, MemoryAttribute attribute, Access access)
{
    if (virtual_address % page_size != 0 || physical_address % page_size != 0 || size % page_size != 0)
        return Error::from_string_literal("unaligned mapping");

    while (size > 0) {
        auto* level1 = TRY(next_level(m_root, table_index(virtual_address, 0)));
        auto* level2 = TRY(next_level(level1, table_index(virtual_address, 1)));

        bool block_fits = virtual_address % level2_block_size == 0 && physical_address % level2_block_size == 0 && size >= level2_block_size;
        u64 step = page_size;
        if (block_fits) {
            TRY(set_leaf(level2[table_index(virtual_address, 2)], leaf_descriptor(physical_address, attribute, access, false)));
            step = level2_block_size;
        } else {
            auto* level3 = TRY(next_level(level2, table_index(virtual_address, 2)));
            TRY(set_leaf(level3[table_index(virtual_address, 3)], leaf_descriptor(physical_address, attribute, access, true)));
        }
        virtual_address += step;
        physical_address += step;
        size -= step;
    }
    return {};
}

}
