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

// The kernel is position independent: what holds an absolute address is listed in its relative
// relocations, applied here before any of it runs.
static ErrorOr<void> apply_relocations(ELF::Image const& image, LoadedKernel const& kernel, u64 lowest)
{
    constexpr u64 relocation_relative = ELF::R_NATIVE_RELATIVE;
    u64 table = 0;
    u64 table_size = 0;
    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_DYNAMIC)
            continue;
        auto data = image.segment_data(segment);
        for (size_t at = 0; at + 16 <= data.size(); at += 16) {
            i64 tag;
            u64 value;
            __builtin_memcpy(&tag, data.data() + at, 8);
            __builtin_memcpy(&value, data.data() + at + 8, 8);
            if (tag == ELF::DT_RELA)
                table = value;
            else if (tag == ELF::DT_RELASZ)
                table_size = value;
        }
    }
    if (table_size == 0)
        return {};
    if (table < lowest || table + table_size > lowest + kernel.size)
        return Error::from_string_literal("kernel relocations outside its image");
    auto* image_start = reinterpret_cast<u8*>(kernel.physical_base);
    for (u64 at = 0; at + 24 <= table_size; at += 24) {
        u64 offset, info, addend;
        __builtin_memcpy(&offset, image_start + (table - lowest) + at, 8);
        __builtin_memcpy(&info, image_start + (table - lowest) + at + 8, 8);
        __builtin_memcpy(&addend, image_start + (table - lowest) + at + 16, 8);
        if ((info & 0xffffffff) != relocation_relative)
            return Error::from_string_literal("kernel has a relocation of an unexpected type");
        if (offset < lowest || offset + 8 > lowest + kernel.size)
            return Error::from_string_literal("kernel relocation outside its image");
        u64 const value = addend + kernel.slide;
        __builtin_memcpy(image_start + (offset - lowest), &value, 8);
    }
    return {};
}

ErrorOr<LoadedKernel> load_kernel(ELF::Image const& image, UEFI::BootServices& boot_services, u64 slide)
{
    auto const& header = image.header();
    if (header.e_machine != ELF::EM_NATIVE)
        return Error::from_string_literal("kernel is not an executable for this processor");
    if (header.e_type != ELF::ET_EXEC && header.e_type != ELF::ET_DYN)
        return Error::from_string_literal("kernel is not an executable");
    // One linked for its address stays there.
    if (header.e_type == ELF::ET_EXEC)
        slide = 0;

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
    kernel.slide = slide;
    kernel.virtual_base = lowest + slide;
    kernel.size = align_up<u64>(highest - lowest, UEFI::page_size);
    kernel.entry = header.e_entry + slide;
    kernel.physical_base = TRY(allocate_pages(boot_services, kernel.size, LoaderMemoryType::KernelImage));

    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_LOAD || segment.p_memsz == 0)
            continue;
        auto data = image.segment_data(segment);
        auto* destination = reinterpret_cast<u8*>(kernel.physical_base + (segment.p_vaddr - lowest));
        __builtin_memcpy(destination, data.data(), data.size());
    }
    TRY(apply_relocations(image, kernel, lowest));
    return kernel;
}

}
