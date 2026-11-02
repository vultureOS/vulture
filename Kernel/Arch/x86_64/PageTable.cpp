/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/BootInfo.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/PageTable.h>
#include <Kernel/Arch/x86_64/MMIO.h>
#include <Kernel/Arch/x86_64/ProcessorArea.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>

// Four levels of 512 entries. The upper half belongs to the kernel, and its top-level entries are the
// same in every process: they are all allocated at the start, and copied into each new table.
//
// Address space identifiers aren't used: each processor flushes what's in its TLB when it changes
// tables, apart from the kernel's, which are global. So only a processor that has a table loaded can
// have anything of it, which means that a change to a table only has to be sent to those.

namespace Kernel::Arch {

namespace {

constexpr u64 present = 1ull << 0;
constexpr u64 writable = 1ull << 1;
constexpr u64 user = 1ull << 2;
constexpr u64 write_through = 1ull << 3;
constexpr u64 cache_disable = 1ull << 4;
constexpr u64 accessed = 1ull << 5;
constexpr u64 dirty = 1ull << 6;
constexpr u64 large = 1ull << 7;
constexpr u64 global = 1ull << 8;
constexpr u64 no_execute = 1ull << 63;
constexpr u64 address_mask = 0x000f'ffff'ffff'f000;

constexpr size_t entries_per_table = 512;
constexpr size_t first_kernel_entry = 256;
constexpr u64 large_2m = 2 * MiB;
constexpr u64 large_1g = 1024 * MiB;

PhysicalAddress s_kernel_root;
bool s_kernel_entries_allocated = false;

size_t table_index(FlatPtr address, unsigned level)
{
    return (address >> (39 - 9 * level)) & 0x1ff;
}

u64* table_pointer(PhysicalAddress address)
{
    return reinterpret_cast<u64*>(API::direct_map_base + address.get());
}

u64* table_pointer_from_entry(u64 entry)
{
    return table_pointer(PhysicalAddress(entry & address_mask));
}

bool is_table(u64 entry, unsigned level)
{
    return level < 3 && (entry & present) && !(entry & large);
}

// See the page attribute table in CPU.cpp: index 0 write-back, 1 write-combining, 3 uncached.
u64 memory_type_bits(MemoryType type)
{
    switch (type) {
    case MemoryType::Normal:
        return 0;
    case MemoryType::NormalUncached:
        return write_through;
    case MemoryType::Device:
        return write_through | cache_disable;
    }
    VERIFY_NOT_REACHED();
}

u64 protection_bits(Protection protection)
{
    switch (protection) {
    case Protection::ReadWrite:
        return writable | no_execute;
    case Protection::ReadOnly:
        return no_execute;
    case Protection::ReadExecute:
        return 0;
    }
    VERIFY_NOT_REACHED();
}

u64 user_page_entry(PhysicalAddress page, Protection protection)
{
    return page.get() | present | user | accessed | dirty | protection_bits(protection);
}

bool has_gigabyte_pages()
{
    return cpuid(0x8000'0000).eax >= 0x8000'0001 && (cpuid(0x8000'0001).edx & (1u << 26));
}

u64 read_root()
{
    u64 root;
    asm volatile("movq %%cr3, %0" : "=r"(root));
    return root & address_mask;
}

void invalidate(FlatPtr address)
{
    asm volatile("invlpg (%0)" ::"r"(address) : "memory");
}

struct Shootdown {
    u64 root; // 0: the kernel's, which everyone has.
    FlatPtr first;
    size_t pages;
};

void shootdown_here(void* argument)
{
    auto const& shootdown = *static_cast<Shootdown const*>(argument);
    if (shootdown.root != 0 && read_root() != shootdown.root)
        return;
    for (size_t i = 0; i < shootdown.pages; ++i)
        invalidate(shootdown.first + i * page_size);
}

// When this returns, no processor is using the old translation of these pages.
void flush_pages(u64 root, FlatPtr first, size_t pages)
{
    Shootdown shootdown { root, first, pages };
    bool anyone_else = false;
    u32 const processors = Processor::count();
    u32 const self = Processor::current().id();
    for (u32 id = 0; id < processors && !anyone_else; ++id) {
        if (id == self || !Processor::with_id(id).is_online())
            continue;
        // (It finds the new entry anyway, if it loads this table after looking here.)
        anyone_else = root == 0 || __atomic_load_n(&X86_64::area_at(id).active_root, __ATOMIC_ACQUIRE) == root;
    }
    if (anyone_else) {
        // (This could wait for ever otherwise: they might be waiting on this processor for something.)
        VERIFY(are_interrupts_enabled());
        Processor::call_on_all(shootdown_here, &shootdown);
    } else {
        shootdown_here(&shootdown);
    }
}

}

ErrorOr<PageTable> PageTable::create()
{
    auto root = TRY(physical_memory().allocate_zeroed_page(PageUse::PageTable));
    // Anything but the first is a user's table, sharing the kernel's half.
    if (s_kernel_root.get() != 0) {
        auto const* kernel = table_pointer(s_kernel_root);
        auto* table = table_pointer(root);
        for (size_t i = first_kernel_entry; i < entries_per_table; ++i)
            table[i] = kernel[i];
    }
    return PageTable(root);
}

ErrorOr<u64*> PageTable::next_level(u64* table, size_t index)
{
    u64 const entry = table[index];
    if (entry & present) {
        if (entry & large)
            return Error::from_string_literal("a large page is in the way");
        return table_pointer_from_entry(entry);
    }
    auto page = TRY(physical_memory().allocate_zeroed_page(PageUse::PageTable));
    ++m_table_pages;
    table[index] = page.get() | present | writable | user;
    return table_pointer(page);
}

ErrorOr<void> PageTable::map(FlatPtr virtual_address, PhysicalAddress physical_address, size_t size, MemoryType type, Protection protection)
{
    VERIFY(virtual_address % page_size == 0 && physical_address.is_page_aligned() && size % page_size == 0);
    bool const gigabytes = has_gigabyte_pages();
    u64 const kernel_bits = virtual_address >= API::direct_map_base ? global : 0;

    while (size > 0) {
        unsigned level = 3;
        u64 block_size = page_size;
        u64 const alignment = virtual_address | physical_address.get();
        if (gigabytes && alignment % large_1g == 0 && size >= large_1g) {
            level = 1;
            block_size = large_1g;
        } else if (alignment % large_2m == 0 && size >= large_2m) {
            level = 2;
            block_size = large_2m;
        }

        u64* table = table_pointer(m_root);
        for (unsigned current = 0; current < level; ++current)
            table = TRY(next_level(table, table_index(virtual_address, current)));

        u64& entry = table[table_index(virtual_address, level)];
        u64 descriptor = physical_address.get() | present | accessed | dirty | kernel_bits | protection_bits(protection) | memory_type_bits(type);
        if (level < 3)
            descriptor |= large;
        if (entry & present) {
            if (entry != descriptor)
                return Error::from_string_literal("conflicting mapping");
        } else {
            entry = descriptor;
        }

        virtual_address += block_size;
        physical_address = physical_address.offset(block_size);
        size -= block_size;
    }
    return {};
}

void PageTable::unmap(FlatPtr virtual_address, size_t size)
{
    VERIFY(virtual_address % page_size == 0 && size % page_size == 0);
    for (FlatPtr address = virtual_address; address < virtual_address + size; address += page_size) {
        u64* table = table_pointer(m_root);
        for (unsigned level = 0; level < 3; ++level) {
            u64 const entry = table[table_index(address, level)];
            VERIFY(is_table(entry, level));
            table = table_pointer_from_entry(entry);
        }
        u64& entry = table[table_index(address, 3)];
        VERIFY(entry & present);
        entry = 0;
    }
    if (virtual_address >= API::direct_map_base) {
        // The kernel's, with a lock held: other processors find out in finish_remapping_kernel_range().
        Shootdown local { 0, virtual_address, size / page_size };
        shootdown_here(&local);
    } else {
        flush_pages(m_root.get(), virtual_address, size / page_size);
    }
}

Optional<PhysicalAddress> PageTable::translate(FlatPtr virtual_address) const
{
    u64 const* table = table_pointer(m_root);
    for (unsigned level = 0; level <= 3; ++level) {
        u64 const entry = table[table_index(virtual_address, level)];
        if (!(entry & present))
            return {};
        if (!is_table(entry, level)) {
            unsigned const shift = 39 - 9 * level;
            u64 const block_mask = (1ull << shift) - 1;
            return PhysicalAddress((entry & address_mask & ~block_mask) | (virtual_address & block_mask));
        }
        table = table_pointer_from_entry(entry);
    }
    return {};
}

// The last-level entry for a user address, if the tables go that far.
static u64* user_entry(PhysicalAddress root, FlatPtr virtual_address)
{
    u64* current = table_pointer(root);
    for (unsigned level = 0; level < 3; ++level) {
        u64 const entry = current[table_index(virtual_address, level)];
        if (!is_table(entry, level))
            return nullptr;
        current = table_pointer_from_entry(entry);
    }
    return &current[table_index(virtual_address, 3)];
}

ErrorOr<void> PageTable::map_user_page(FlatPtr virtual_address, PhysicalAddress physical_address, Protection protection)
{
    VERIFY(virtual_address % page_size == 0 && physical_address.is_page_aligned());
    u64* table = table_pointer(m_root);
    for (unsigned level = 0; level < 3; ++level)
        table = TRY(next_level(table, table_index(virtual_address, level)));
    u64& entry = table[table_index(virtual_address, 3)];
    u64 const descriptor = user_page_entry(physical_address, protection);
    if (entry == descriptor)
        return {};
    u64 const previous = entry;
    entry = descriptor;
    // (Nothing can hold a translation of an entry that wasn't present.)
    if (previous & present)
        flush_pages(m_root.get(), virtual_address, 1);
    return {};
}

Optional<PhysicalAddress> PageTable::unmap_user_page(FlatPtr virtual_address)
{
    u64* entry = user_entry(m_root, virtual_address);
    if (!entry || !(*entry & present))
        return {};
    auto const physical = PhysicalAddress(*entry & address_mask);
    *entry = 0;
    flush_pages(m_root.get(), virtual_address, 1);
    return physical;
}

bool PageTable::protect_user_page(FlatPtr virtual_address, Protection protection)
{
    u64* entry = user_entry(m_root, virtual_address);
    if (!entry || !(*entry & present))
        return false;
    u64 const descriptor = user_page_entry(PhysicalAddress(*entry & address_mask), protection);
    if (*entry != descriptor) {
        *entry = descriptor;
        flush_pages(m_root.get(), virtual_address, 1);
    }
    return true;
}

Optional<Protection> PageTable::user_page_protection(FlatPtr virtual_address) const
{
    u64 const* entry = user_entry(m_root, virtual_address);
    if (!entry || !(*entry & present))
        return {};
    if (*entry & writable)
        return Protection::ReadWrite;
    return (*entry & no_execute) ? Protection::ReadOnly : Protection::ReadExecute;
}

static void free_tables(u64* table, unsigned level, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        if (is_table(table[index], level)) {
            free_tables(table_pointer_from_entry(table[index]), level + 1, entries_per_table);
            physical_memory().free_pages(PhysicalAddress(table[index] & address_mask));
        }
    }
}

void PageTable::destroy()
{
    // The kernel's half is the kernel's.
    free_tables(table_pointer(m_root), 0, first_kernel_entry);
    physical_memory().free_pages(m_root);
    m_root = {};
    m_table_pages = 0;
}

void finish_remapping_kernel_range(FlatPtr virtual_address, size_t size)
{
    flush_pages(0, virtual_address, size / page_size);
}

void activate_kernel_page_table(PageTable const& table)
{
    s_kernel_root = table.root();
    asm volatile("movq %0, %%cr3" ::"r"(table.root().get()) : "memory");
    // Taking in anything global the loader's tables may have left.
    u64 cr4;
    asm volatile("movq %%cr4, %0\n"
                 "movq %0, %%cr4" : "=r"(cr4));
    X86_64::area_at(Processor::current().id()).active_root = table.root().get();
}

ErrorOr<void> initialize_user_translation()
{
    // Every entry of the kernel's half exists from here on, and they are the same ones in all tables.
    VERIFY(!s_kernel_entries_allocated);
    auto* table = table_pointer(s_kernel_root);
    for (size_t i = first_kernel_entry; i < entries_per_table; ++i) {
        if (table[i] & present)
            continue;
        auto page = TRY(physical_memory().allocate_zeroed_page(PageUse::PageTable));
        table[i] = page.get() | present | writable;
    }
    s_kernel_entries_allocated = true;
    activate_user_page_table(nullptr, 0);
    return {};
}

ErrorOr<u16> allocate_address_space_id()
{
    return static_cast<u16>(1);
}

void free_address_space_id(u16)
{
}

void activate_user_page_table(PageTable const* table, u16)
{
    u64 const root = table ? table->root().get() : s_kernel_root.get();
    VERIFY(root);
    auto& area = X86_64::area_at(Processor::current().id());
    // Before the load: anyone who sees the old value here finds the new table entry when it loads it,
    // and anyone changing the table after this sees this.
    __atomic_store_n(&area.active_root, root, __ATOMIC_SEQ_CST);
    if (read_root() != root)
        asm volatile("movq %0, %%cr3" ::"r"(root) : "memory");
}

}
