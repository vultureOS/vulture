/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/Error.h>

namespace Kernel::Arch {

enum class MemoryType : u8 {
    Normal, // RAM: cached.
    Device, // MMIO registers: uncached, accesses neither merged nor reordered.
    NormalUncached, // Uncached RAM-like memory, such as a framebuffer outside RAM.
};

enum class Protection : u8 {
    ReadWrite,
    ReadOnly,
    ReadExecute,
};

// A tree of translation tables: the kernel's (upper half), or a process's
// user space (lower half), which is mapped one 4 KiB page at a time.
class PageTable {
public:
    PageTable() = default;

    [[nodiscard]] static ErrorOr<PageTable> create();

    // Maps [virtual_address, virtual_address + size), using large blocks
    // where alignment allows. Remapping a page identically is allowed;
    // changing an existing mapping is an error.
    ErrorOr<void> map(FlatPtr virtual_address, PhysicalAddress, size_t size, MemoryType, Protection);

    // Removes page (not block) mappings and flushes them from every
    // processor's TLB.
    void unmap(FlatPtr virtual_address, size_t size);

    [[nodiscard]] Optional<PhysicalAddress> translate(FlatPtr virtual_address) const;

    // User pages. Replacing or changing a mapping flushes the old one from
    // every processor's TLB first.
    ErrorOr<void> map_user_page(FlatPtr virtual_address, PhysicalAddress, Protection);
    // Returns what was mapped, if anything.
    Optional<PhysicalAddress> unmap_user_page(FlatPtr virtual_address);
    // Returns false if nothing is mapped there.
    bool protect_user_page(FlatPtr virtual_address, Protection);
    [[nodiscard]] Optional<Protection> user_page_protection(FlatPtr virtual_address) const;

    // Frees the tables themselves (not the memory they map).
    void destroy();

    [[nodiscard]] PhysicalAddress root() const { return m_root; }
    [[nodiscard]] size_t table_page_count() const { return m_table_pages; }

private:
    explicit PageTable(PhysicalAddress root)
        : m_root(root)
        , m_table_pages(1)
    {
    }

    ErrorOr<u64*> next_level(u64* table, size_t index);

    PhysicalAddress m_root;
    size_t m_table_pages { 0 };
};

// Makes `table` this processor's kernel (upper-half) translation table.
void activate_kernel_page_table(PageTable const&);

// Address space identifiers (ASIDs) tag user TLB entries, so switching
// between processes needs no TLB flush.
ErrorOr<u16> allocate_address_space_id();
// Flushes the ASID's entries from every processor, then frees it.
void free_address_space_id(u16);

// Once, on the boot processor: an empty user table for kernel threads, and
// 16-bit ASIDs if the CPU has them.
ErrorOr<void> initialize_user_translation();

// Makes `table` (tagged `asid`) this processor's user space; with null,
// user space is empty (kernel threads).
void activate_user_page_table(PageTable const*, u16 asid);

}
