/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Memory.h>

namespace Boot {

using Kernel::API::MemoryRegion;
using Kernel::API::MemoryRegionType;

ErrorOr<UEFI::PhysicalAddress> allocate_pages(UEFI::BootServices& boot_services, size_t size, LoaderMemoryType type)
{
    u64 pages = align_up<u64>(max<u64>(size, 1), UEFI::page_size) / UEFI::page_size;
    UEFI::PhysicalAddress address = 0;
    auto status = boot_services.AllocatePages(UEFI::AllocateType::AnyPages, static_cast<UEFI::MemoryType>(type), pages, &address);
    if (UEFI::is_error(status))
        return Error::from_string_literal("AllocatePages failed");
    __builtin_memset(reinterpret_cast<void*>(address), 0, pages * UEFI::page_size);
    return address;
}

ErrorOr<FirmwareMemoryMap> FirmwareMemoryMap::create(UEFI::BootServices& boot_services)
{
    FirmwareMemoryMap map;
    UEFI::UIntN size = 0;
    u32 descriptor_version = 0;
    auto status = boot_services.GetMemoryMap(&size, nullptr, &map.m_map_key, &map.m_descriptor_size, &descriptor_version);
    if (status != UEFI::StatusCode::BufferTooSmall || map.m_descriptor_size < sizeof(UEFI::MemoryDescriptor))
        return Error::from_string_literal("GetMemoryMap failed");

    // Leave room for the map to grow: our own allocations split regions.
    map.m_capacity = size + 64 * map.m_descriptor_size;
    void* buffer = nullptr;
    if (UEFI::is_error(boot_services.AllocatePool(UEFI::MemoryType::LoaderData, map.m_capacity, &buffer)))
        return Error::from_string_literal("AllocatePool failed for the memory map");
    map.m_buffer = static_cast<u8*>(buffer);
    TRY(map.refresh(boot_services));
    return map;
}

ErrorOr<void> FirmwareMemoryMap::refresh(UEFI::BootServices& boot_services)
{
    UEFI::UIntN size = m_capacity;
    u32 descriptor_version = 0;
    auto status = boot_services.GetMemoryMap(&size, reinterpret_cast<UEFI::MemoryDescriptor*>(m_buffer), &m_map_key, &m_descriptor_size, &descriptor_version);
    if (UEFI::is_error(status))
        return Error::from_string_literal("GetMemoryMap failed");
    m_size = size;
    return {};
}

UEFI::MemoryDescriptor const& FirmwareMemoryMap::descriptor(size_t index) const
{
    VERIFY(index < descriptor_count());
    return *reinterpret_cast<UEFI::MemoryDescriptor const*>(m_buffer + index * m_descriptor_size);
}

MemoryRegionType region_type(u32 firmware_type)
{
    switch (firmware_type) {
    case to_underlying(LoaderMemoryType::KernelImage):
        return MemoryRegionType::KernelImage;
    case to_underlying(LoaderMemoryType::KernelFile):
        return MemoryRegionType::KernelFile;
    case to_underlying(LoaderMemoryType::BootArchive):
        return MemoryRegionType::BootArchive;
    case to_underlying(LoaderMemoryType::LowMemory):
        return MemoryRegionType::LowMemory;
    case to_underlying(LoaderMemoryType::LoaderReclaimable):
        return MemoryRegionType::LoaderReclaimable;
    }

    switch (static_cast<UEFI::MemoryType>(firmware_type)) {
    // Everything the firmware and this loader leave behind is free once the kernel runs.
    case UEFI::MemoryType::ConventionalMemory:
    case UEFI::MemoryType::BootServicesCode:
    case UEFI::MemoryType::BootServicesData:
    case UEFI::MemoryType::LoaderCode:
    case UEFI::MemoryType::LoaderData:
        return MemoryRegionType::Usable;
    case UEFI::MemoryType::RuntimeServicesCode:
        return MemoryRegionType::RuntimeServicesCode;
    case UEFI::MemoryType::RuntimeServicesData:
        return MemoryRegionType::RuntimeServicesData;
    case UEFI::MemoryType::ACPIReclaimMemory:
        return MemoryRegionType::AcpiReclaimable;
    case UEFI::MemoryType::ACPIMemoryNVS:
        return MemoryRegionType::AcpiNvs;
    case UEFI::MemoryType::MemoryMappedIO:
    case UEFI::MemoryType::MemoryMappedIOPortSpace:
        return MemoryRegionType::Mmio;
    default:
        return MemoryRegionType::Reserved;
    }
}

size_t convert_memory_map(FirmwareMemoryMap const& map, Span<MemoryRegion> output)
{
    size_t count = 0;
    for (size_t i = 0; i < map.descriptor_count() && count < output.size(); ++i) {
        auto const& descriptor = map.descriptor(i);
        output[count++] = { descriptor.PhysicalStart, descriptor.NumberOfPages * UEFI::page_size, region_type(descriptor.Type), 0 };
    }

    // Insertion sort: the map holds at most a few hundred entries.
    for (size_t i = 1; i < count; ++i) {
        auto region = output[i];
        size_t j = i;
        while (j > 0 && output[j - 1].base > region.base) {
            output[j] = output[j - 1];
            --j;
        }
        output[j] = region;
    }

    size_t merged = 0;
    for (size_t i = 0; i < count; ++i) {
        if (merged > 0) {
            auto& previous = output[merged - 1];
            if (previous.type == output[i].type && previous.base + previous.length == output[i].base) {
                previous.length += output[i].length;
                continue;
            }
        }
        output[merged++] = output[i];
    }
    return merged;
}

}
