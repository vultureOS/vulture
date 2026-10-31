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

// A tree of translation tables. Mappings are kernel-only: user access
// arrives with address spaces (M4).
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

}
