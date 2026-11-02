/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// AArch64 VMSAv8-64 translation table format for a 4 KiB granule and 48-bit
// virtual addresses (four levels, 0-3). VultureBoot builds the first tables
// with these definitions and the kernel keeps using the same memory
// attribute layout, so both include this header.

namespace Kernel::AArch64 {

constexpr u64 page_size = 4 * KiB;
constexpr u64 entries_per_table = 512;

// Bytes covered by one entry at each level.
constexpr u64 level1_block_size = 1 * GiB;
constexpr u64 level2_block_size = 2 * MiB;

constexpr size_t table_index(u64 virtual_address, unsigned level)
{
    return (virtual_address >> (39 - 9 * level)) & (entries_per_table - 1);
}

// MAIR_EL1 attribute slots. The index goes in a descriptor's AttrIndx field.
enum class MemoryAttribute : u64 {
    Normal = 0, // Normal, inner/outer write-back, read/write-allocate.
    Device = 1, // Device-nGnRE: MMIO registers.
    NormalUncached = 2, // Normal, non-cacheable: framebuffers.
};

constexpr u64 mair_value = (u64(0xff) << 0) | (u64(0x04) << 8) | (u64(0x44) << 16);

namespace Descriptor {
constexpr u64 valid = 1ull << 0;
constexpr u64 table = 1ull << 1; // At levels 0-2: points to a table. At level 3: a page.
constexpr u64 page = 1ull << 1;
constexpr u64 attribute_index_shift = 2;
constexpr u64 user_access = 1ull << 6; // AP[1]: EL0 may access.
constexpr u64 read_only = 1ull << 7; // AP[2]: no writes at any level.
constexpr u64 inner_shareable = 3ull << 8;
constexpr u64 access_flag = 1ull << 10;
constexpr u64 not_global = 1ull << 11; // Tagged with the ASID (user mappings).
constexpr u64 guarded = 1ull << 50; // BTI: indirect branches here must land on a landing pad.
constexpr u64 privileged_execute_never = 1ull << 53;
constexpr u64 unprivileged_execute_never = 1ull << 54;
constexpr u64 address_mask = 0x0000'ffff'ffff'f000;
}

enum class Access {
    ReadWrite,
    ReadOnly,
    ReadExecute,
};

// A level-3 page or level-1/2 block entry for kernel memory. EL0 never gets
// access or execute rights here.
constexpr u64 leaf_descriptor(u64 physical_address, MemoryAttribute attribute, Access access, bool is_page)
{
    using namespace Descriptor;
    u64 entry = (physical_address & address_mask) | valid | access_flag | unprivileged_execute_never;
    entry |= static_cast<u64>(attribute) << attribute_index_shift;
    if (attribute == MemoryAttribute::Normal || attribute == MemoryAttribute::NormalUncached)
        entry |= inner_shareable;
    if (access != Access::ReadWrite)
        entry |= read_only;
    if (access != Access::ReadExecute)
        entry |= privileged_execute_never;
    if (is_page)
        entry |= page;
    return entry;
}

// A level-3 page for user memory (Normal, cacheable): accessible at EL0,
// tagged with the address space's ASID, never executable by the kernel.
constexpr u64 user_page_descriptor(u64 physical_address, Access access)
{
    using namespace Descriptor;
    u64 entry = (physical_address & address_mask) | valid | page | access_flag | inner_shareable | not_global | user_access | privileged_execute_never;
    entry |= static_cast<u64>(MemoryAttribute::Normal) << attribute_index_shift;
    if (access != Access::ReadWrite)
        entry |= read_only;
    if (access != Access::ReadExecute)
        entry |= unprivileged_execute_never;
    return entry;
}

// The kernel half uses 16-bit ASIDs in TTBR0 when the CPU has them.
constexpr u64 tcr_asid_16_bits = 1ull << 36;
constexpr u64 tcr_epd0 = 1ull << 7;

constexpr u64 table_descriptor(u64 table_physical_address)
{
    return (table_physical_address & Descriptor::address_mask) | Descriptor::valid | Descriptor::table;
}

// TCR_EL1 for 48-bit addresses in both halves, 4 KiB granules, and cacheable,
// inner-shareable table walks. `physical_address_size` is the PARange field
// of ID_AA64MMFR0_EL1.
constexpr u64 tcr_value(u64 physical_address_size)
{
    constexpr u64 t0sz = 16;
    constexpr u64 t1sz = 16 << 16;
    constexpr u64 walk_attributes_ttbr0 = (1ull << 8) | (1ull << 10) | (3ull << 12); // IRGN0, ORGN0: write-back; SH0: inner
    constexpr u64 walk_attributes_ttbr1 = (1ull << 24) | (1ull << 26) | (3ull << 28); // IRGN1, ORGN1, SH1
    constexpr u64 granule_4k_ttbr1 = 2ull << 30; // TG1 (TG0 = 0 is 4 KiB already)
    // Tables here only produce 48-bit output addresses, so cap IPS at 48 bits.
    u64 ips = physical_address_size > 5 ? 5 : physical_address_size;
    return t0sz | t1sz | walk_attributes_ttbr0 | walk_attributes_ttbr1 | granule_4k_ttbr1 | (ips << 32);
}

}
