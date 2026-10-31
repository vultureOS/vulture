/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Boot/Memory.h>
#include <Kernel/Arch/aarch64/PageTables.h>
#include <LibBase/Array.h>

namespace Boot::AArch64 {

using Kernel::AArch64::Access;
using Kernel::AArch64::MemoryAttribute;

// Builds a set of 4-level translation tables in memory the kernel keeps
// (LoaderReclaimable). Uses 2 MiB blocks wherever alignment allows, 4 KiB
// pages elsewhere.
class PageTableBuilder {
public:
    static ErrorOr<PageTableBuilder> create(UEFI::BootServices&);

    // Maps [virtual_address, virtual_address + size). Mapping the same page
    // twice with identical attributes is allowed; conflicting mappings fail.
    ErrorOr<void> map(u64 virtual_address, u64 physical_address, u64 size, MemoryAttribute, Access);

    [[nodiscard]] u64 root() const { return reinterpret_cast<u64>(m_root); }

    // Every table page, so they can be written back to memory before the MMU switch.
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
