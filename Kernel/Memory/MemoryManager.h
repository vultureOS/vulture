/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/BootInfo.h>
#include <Kernel/Arch/MemoryLayout.h>
#include <Kernel/Arch/PageTable.h>
#include <Kernel/Locking/SpinLock.h>
#include <LibBase/Span.h>

namespace Kernel {

struct KernelStack {
    FlatPtr bottom { 0 };
    FlatPtr top { 0 };
};

// The kernel's address space: its own page tables (replacing the loader's),
// the direct map of RAM, device mappings and kernel stacks.
class MemoryManager {
public:
    // Brings up physical memory, copies what the kernel keeps from the
    // loader's memory, and builds the kernel's page tables. They take effect
    // with activate_kernel_page_table().
    ErrorOr<void> initialize(API::BootInfo const&);

    // Switches the current processor to the kernel's page tables. Device
    // mappings made before the switch are the only valid ones afterwards.
    void activate_kernel_page_table();

    // Once nothing refers to the loader's memory any more: hands it to the
    // physical memory manager.
    void reclaim_boot_memory();

    // Maps device memory and returns the virtual address of `address`.
    ErrorOr<FlatPtr> map_mmio(PhysicalAddress, size_t size, Arch::MemoryType = Arch::MemoryType::Device);

    // A kernel stack with an unmapped guard area below it.
    ErrorOr<KernelStack> allocate_kernel_stack();
    void free_kernel_stack(KernelStack);

    [[nodiscard]] Arch::PageTable const& kernel_page_table() const { return m_kernel_page_table; }

    // The kernel's copies, valid after the loader's memory is reclaimed.
    [[nodiscard]] Span<API::MemoryRegion const> memory_map() const { return { m_memory_map, m_memory_map_entries }; }
    [[nodiscard]] ReadonlyBytes device_tree() const { return m_device_tree; }

    // Where a kernel image address (code or static data) is in physical memory.
    [[nodiscard]] PhysicalAddress physical_address_of_kernel_image(void const*) const;

    [[nodiscard]] size_t reclaimed_bytes() const { return m_reclaimed_bytes; }

private:
    ErrorOr<void> map_kernel_image();
    ErrorOr<void> map_physical_memory();
    ErrorOr<Bytes> copy_to_new_pages(ReadonlyBytes);
    ErrorOr<void> map_new_kernel_page(FlatPtr);

    SpinLock m_lock;
    Arch::PageTable m_kernel_page_table;
    FlatPtr m_next_mmio_address { 0 };
    u8 m_stack_slots_in_use[MemoryLayout::kernel_stack_slots / 8] {};

    API::MemoryRegion const* m_memory_map { nullptr };
    size_t m_memory_map_entries { 0 };
    ReadonlyBytes m_device_tree;
    API::PhysicalRange m_kernel_image {};
    FlatPtr m_kernel_virtual_base { 0 };
    size_t m_reclaimed_bytes { 0 };
};

MemoryManager& memory_manager();

template<typename T = u8>
T* physical_to_virtual(PhysicalAddress address)
{
    return reinterpret_cast<T*>(API::direct_map_base + address.get());
}

// For direct map addresses only.
inline PhysicalAddress virtual_to_physical(void const* pointer)
{
    auto address = reinterpret_cast<FlatPtr>(pointer);
    VERIFY(address >= API::direct_map_base && address < API::direct_map_base + (1ull << 47));
    return PhysicalAddress(address - API::direct_map_base);
}

}
