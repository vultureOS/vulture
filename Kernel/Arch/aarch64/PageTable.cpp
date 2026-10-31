/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/BootInfo.h>
#include <Kernel/Arch/PageTable.h>
#include <Kernel/Arch/aarch64/PageTables.h>
#include <Kernel/Memory/PhysicalMemory.h>

namespace Kernel::Arch {

using AArch64::Access;
using AArch64::MemoryAttribute;
using AArch64::table_index;
namespace Descriptor = AArch64::Descriptor;

static u64* table_pointer(PhysicalAddress address)
{
    return reinterpret_cast<u64*>(API::direct_map_base + address.get());
}

static u64* table_pointer_from_entry(u64 entry)
{
    return table_pointer(PhysicalAddress(entry & Descriptor::address_mask));
}

static MemoryAttribute attribute_for(MemoryType type)
{
    switch (type) {
    case MemoryType::Normal:
        return MemoryAttribute::Normal;
    case MemoryType::Device:
        return MemoryAttribute::Device;
    case MemoryType::NormalUncached:
        return MemoryAttribute::NormalUncached;
    }
    VERIFY_NOT_REACHED();
}

static Access access_for(Protection protection)
{
    switch (protection) {
    case Protection::ReadWrite:
        return Access::ReadWrite;
    case Protection::ReadOnly:
        return Access::ReadOnly;
    case Protection::ReadExecute:
        return Access::ReadExecute;
    }
    VERIFY_NOT_REACHED();
}

// At levels 0-2 an entry is a table or a block; level 3 only has pages
// (which reuse the "table" bit).
static bool is_table(u64 entry, unsigned level)
{
    return level < 3 && (entry & Descriptor::valid) && (entry & Descriptor::table);
}

ErrorOr<PageTable> PageTable::create()
{
    auto root = TRY(physical_memory().allocate_zeroed_page(PageUse::PageTable));
    return PageTable(root);
}

ErrorOr<u64*> PageTable::next_level(u64* table, size_t index)
{
    u64 entry = table[index];
    if (entry & Descriptor::valid) {
        if (!(entry & Descriptor::table))
            return Error::from_string_literal("a block mapping is in the way");
        return table_pointer_from_entry(entry);
    }
    auto page = TRY(physical_memory().allocate_zeroed_page(PageUse::PageTable));
    ++m_table_pages;
    // The zeroed table must reach memory before a walker can find it.
    asm volatile("dsb ishst" ::: "memory");
    table[index] = AArch64::table_descriptor(page.get());
    return table_pointer(page);
}

ErrorOr<void> PageTable::map(FlatPtr virtual_address, PhysicalAddress physical_address, size_t size, MemoryType type, Protection protection)
{
    VERIFY(virtual_address % page_size == 0 && physical_address.is_page_aligned() && size % page_size == 0);
    auto attribute = attribute_for(type);
    auto access = access_for(protection);

    while (size > 0) {
        unsigned level = 3;
        u64 block_size = page_size;
        u64 alignment = virtual_address | physical_address.get();
        if (alignment % AArch64::level1_block_size == 0 && size >= AArch64::level1_block_size) {
            level = 1;
            block_size = AArch64::level1_block_size;
        } else if (alignment % AArch64::level2_block_size == 0 && size >= AArch64::level2_block_size) {
            level = 2;
            block_size = AArch64::level2_block_size;
        }

        u64* table = table_pointer(m_root);
        for (unsigned current = 0; current < level; ++current)
            table = TRY(next_level(table, table_index(virtual_address, current)));

        u64& entry = table[table_index(virtual_address, level)];
        u64 descriptor = AArch64::leaf_descriptor(physical_address.get(), attribute, access, level == 3);
        if (entry & Descriptor::valid) {
            if (entry != descriptor)
                return Error::from_string_literal("conflicting mapping");
        } else {
            entry = descriptor;
        }

        virtual_address += block_size;
        physical_address = physical_address.offset(block_size);
        size -= block_size;
    }
    // A new mapping replaces an invalid entry, which no TLB holds, so no
    // invalidation is needed; only ordering.
    asm volatile("dsb ishst\n"
                 "isb" ::
                     : "memory");
    return {};
}

void PageTable::unmap(FlatPtr virtual_address, size_t size)
{
    VERIFY(virtual_address % page_size == 0 && size % page_size == 0);
    for (FlatPtr address = virtual_address; address < virtual_address + size; address += page_size) {
        u64* table = table_pointer(m_root);
        for (unsigned level = 0; level < 3; ++level) {
            u64 entry = table[table_index(address, level)];
            VERIFY(is_table(entry, level));
            table = table_pointer_from_entry(entry);
        }
        u64& entry = table[table_index(address, 3)];
        VERIFY(entry & Descriptor::valid);
        entry = 0;
    }
    // Broadcast invalidation: every processor in the inner-shareable
    // domain drops the entries, with no interrupts needed.
    asm volatile("dsb ishst" ::: "memory");
    for (FlatPtr address = virtual_address; address < virtual_address + size; address += page_size) {
        u64 operand = (address >> 12) & ((1ull << 44) - 1);
        asm volatile("tlbi vaale1is, %0" ::"r"(operand) : "memory");
    }
    asm volatile("dsb ish\n"
                 "isb" ::
                     : "memory");
}

Optional<PhysicalAddress> PageTable::translate(FlatPtr virtual_address) const
{
    u64 const* table = table_pointer(m_root);
    for (unsigned level = 0; level <= 3; ++level) {
        u64 entry = table[table_index(virtual_address, level)];
        if (!(entry & Descriptor::valid))
            return {};
        if (!is_table(entry, level)) {
            unsigned shift = 39 - 9 * level;
            u64 block_mask = (1ull << shift) - 1;
            return PhysicalAddress((entry & Descriptor::address_mask & ~block_mask) | (virtual_address & block_mask));
        }
        table = table_pointer_from_entry(entry);
    }
    return {};
}

static void free_tables(u64* table, unsigned level)
{
    for (size_t index = 0; index < AArch64::entries_per_table; ++index) {
        if (is_table(table[index], level)) {
            free_tables(table_pointer_from_entry(table[index]), level + 1);
            physical_memory().free_pages(PhysicalAddress(table[index] & Descriptor::address_mask));
        }
    }
}

void PageTable::destroy()
{
    free_tables(table_pointer(m_root), 0);
    physical_memory().free_pages(m_root);
    m_root = {};
    m_table_pages = 0;
}

void activate_kernel_page_table(PageTable const& table)
{
    // One sequence, with no memory accesses in between: the old and new
    // tables map the kernel identically, so execution carries on seamlessly.
    asm volatile("dsb ishst\n"
                 "msr ttbr1_el1, %0\n"
                 "isb\n"
                 "tlbi vmalle1\n"
                 "dsb nsh\n"
                 "isb" ::"r"(table.root().get())
        : "memory");
}

}
