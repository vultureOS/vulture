/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/KernelLoader.h>
#include <LibBase/NumericLimits.h>

namespace Boot {

// The kernel must live in the upper half of the address space, away from
// the direct map at its bottom.
static constexpr u64 kernel_space_start = 0xffff'8000'0000'0000;

ErrorOr<LoadedKernel> load_kernel(ELF::Image const& image, UEFI::BootServices& boot_services)
{
    auto const& header = image.header();
    if (header.e_machine != ELF::EM_AARCH64)
        return Error::from_string_literal("kernel is not an AArch64 executable");
    if (header.e_type != ELF::ET_EXEC)
        return Error::from_string_literal("kernel is not a static executable");

    u64 lowest = NumericLimits<u64>::max();
    u64 highest = 0;
    bool entry_is_executable = false;
    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_LOAD || segment.p_memsz == 0)
            continue;
        u64 end = 0;
        if (__builtin_add_overflow(segment.p_vaddr, segment.p_memsz, &end) || segment.p_vaddr < kernel_space_start)
            return Error::from_string_literal("kernel segment outside kernel space");
        if (segment.p_vaddr % UEFI::page_size != 0)
            return Error::from_string_literal("kernel segment is not page-aligned");
        lowest = min(lowest, segment.p_vaddr);
        highest = max(highest, end);
        if ((segment.p_flags & ELF::PF_X) && header.e_entry >= segment.p_vaddr && header.e_entry < end)
            entry_is_executable = true;
    }
    if (highest == 0)
        return Error::from_string_literal("kernel has no loadable segments");
    if (!entry_is_executable)
        return Error::from_string_literal("kernel entry point is not in an executable segment");

    LoadedKernel kernel;
    kernel.virtual_base = lowest;
    kernel.size = align_up<u64>(highest - lowest, UEFI::page_size);
    kernel.entry = header.e_entry;
    kernel.physical_base = TRY(allocate_pages(boot_services, kernel.size, LoaderMemoryType::KernelImage));

    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_LOAD || segment.p_memsz == 0)
            continue;
        auto data = image.segment_data(segment);
        auto* destination = reinterpret_cast<u8*>(kernel.physical_base + (segment.p_vaddr - lowest));
        __builtin_memcpy(destination, data.data(), data.size());
    }
    return kernel;
}

}
