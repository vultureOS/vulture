/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Boot/UEFI/UEFI.h>
#include <Kernel/API/BootInfo.h>
#include <LibBase/Error.h>
#include <LibBase/Span.h>

namespace Boot {

// Memory types from the range UEFI reserves for OS loaders. Tagging our
// allocations this way lets the final memory map tell the kernel which
// memory holds its image, its file and the handoff data.
enum class LoaderMemoryType : u32 {
    KernelImage = 0x80000000,
    KernelFile,
    BootArchive,
    LoaderReclaimable,
};

// Allocates zeroed whole pages. Firmware identity-maps memory, so the
// returned physical address also works as a pointer until the handoff.
ErrorOr<UEFI::PhysicalAddress> allocate_pages(UEFI::BootServices&, size_t size, LoaderMemoryType);

// The firmware memory map, in a buffer allocated up front so it can be
// fetched again right before ExitBootServices() without allocating.
class FirmwareMemoryMap {
public:
    static ErrorOr<FirmwareMemoryMap> create(UEFI::BootServices&);

    ErrorOr<void> refresh(UEFI::BootServices&);

    [[nodiscard]] size_t descriptor_count() const { return m_size / m_descriptor_size; }
    [[nodiscard]] size_t descriptor_capacity() const { return m_capacity / m_descriptor_size; }
    [[nodiscard]] UEFI::MemoryDescriptor const& descriptor(size_t index) const;
    [[nodiscard]] UEFI::UIntN map_key() const { return m_map_key; }

private:
    FirmwareMemoryMap() = default;

    u8* m_buffer { nullptr };
    size_t m_capacity { 0 };
    size_t m_size { 0 };
    UEFI::UIntN m_map_key { 0 };
    UEFI::UIntN m_descriptor_size { 0 };
};

[[nodiscard]] Kernel::API::MemoryRegionType region_type(u32 firmware_type);

// Converts the firmware map into the kernel's format, sorted by address with
// adjacent regions of the same type merged. Returns the number of regions.
// Runs after ExitBootServices(), so it must not call the firmware.
size_t convert_memory_map(FirmwareMemoryMap const&, Span<Kernel::API::MemoryRegion> output);

}
