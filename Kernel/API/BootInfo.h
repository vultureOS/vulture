/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// The handoff from VultureBoot to the kernel; see Documentation/BootProtocol.md.
// Shared by the loader (a PE image) and the kernel (an ELF image), so it uses
// fixed-width fields only. Addresses are physical unless named otherwise; the
// kernel reaches them through the direct map.

namespace Kernel::API {

constexpr u64 boot_info_magic = 0x464e49544f4f4256; // "VBOOTINF"
constexpr u32 boot_info_version = 2;

// All of physical memory is mapped here, so physical address P is at
// direct_map_base + P.
constexpr u64 direct_map_base = 0xffff'0000'0000'0000;

enum class MemoryRegionType : u32 {
    Usable = 1,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    Mmio,
    RuntimeServicesCode,
    RuntimeServicesData,
    KernelImage,
    KernelFile,
    BootArchive,
    LoaderReclaimable,
};

struct MemoryRegion {
    u64 base;
    u64 length;
    MemoryRegionType type;
    u32 reserved;
};

struct PhysicalRange {
    u64 base;
    u64 size;
};

enum class PixelFormat : u32 {
    None,
    BGRX8888, // Byte order in memory: blue, green, red, unused.
    RGBX8888,
};

struct FramebufferInfo {
    u64 physical_base;
    u64 size;
    u32 width;
    u32 height;
    u32 pitch; // Bytes per row.
    PixelFormat format;
};

enum class SerialType : u32 {
    None,
    PL011,
};

struct SerialInfo {
    u64 physical_base;
    SerialType type;
    u32 reserved;
};

struct BootInfo {
    u64 magic;
    u32 version;
    u32 size;

    // Sorted by base, adjacent regions of the same type merged.
    u64 memory_map;
    u64 memory_map_entries;

    // The loaded segments, and the ELF file itself (kept for its symbol table).
    PhysicalRange kernel_image;
    u64 kernel_virtual_base;
    // How far that is from where it was linked (what addresses in kernel_file are relative to).
    u64 kernel_slide;
    PhysicalRange kernel_file;

    PhysicalRange boot_archive; // Empty until the boot archive exists (M4).
    PhysicalRange device_tree; // A copy made by the loader; empty if none.
    u64 acpi_rsdp; // 0 if the firmware has no ACPI tables.
    u64 uefi_system_table;

    FramebufferInfo framebuffer; // format is None without a linear framebuffer.
    SerialInfo serial;

    u64 entropy_size; // Valid bytes in `entropy` (0 if the firmware has no RNG).
    u8 entropy[64];

    char command_line[256]; // Null-terminated contents of the boot-args file.
};

static_assert(sizeof(MemoryRegion) == 24);
// Pins the layout: the PE loader and the ELF kernel must agree on it.
static_assert(sizeof(BootInfo) == 504);

}
