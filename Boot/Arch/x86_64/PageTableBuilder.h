/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Boot/Memory.h>
#include <LibBase/Array.h>
#include <LibBase/Error.h>
#include <LibBase/Span.h>

namespace Boot::X86_64 {

enum class MemoryAttribute : u8 {
    Normal,
    Device,
    NormalUncached,
};

enum class Access : u8 {
    ReadWrite,
    ReadOnly,
    ReadExecute,
};

// Builds a set of 4-level page tables in memory the kernel keeps (LoaderReclaimable). Uses 2 MiB pages
// wherever alignment allows, 4 KiB ones elsewhere.
class PageTableBuilder {
public:
    static ErrorOr<PageTableBuilder> create(UEFI::BootServices&);

    // Maps [virtual_address, virtual_address + size). Mapping the same page
    // twice with identical attributes is allowed; conflicting mappings fail.
    ErrorOr<void> map(u64 virtual_address, u64 physical_address, u64 size, MemoryAttribute, Access);

    [[nodiscard]] u64 root() const { return reinterpret_cast<u64>(m_root); }
    [[nodiscard]] Span<u64 const> table_pages() const { return { m_table_pages.data(), m_table_page_count }; }

private:
    PageTableBuilder() = default;

    ErrorOr<u64*> allocate_table();
    ErrorOr<u64*> next_level(u64* table, size_t index);
    ErrorOr<void> set_leaf(u64& entry, u64 value);

    UEFI::BootServices* m_boot_services { nullptr };
    u64* m_root { nullptr };
    Array<u64, 512> m_table_pages {};
    size_t m_table_page_count { 0 };
};

}
