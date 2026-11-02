/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Boot/Memory.h>
#include <LibELF/Image.h>

namespace Boot {

struct LoadedKernel {
    UEFI::PhysicalAddress physical_base { 0 };
    u64 virtual_base { 0 }; // Address of the lowest segment.
    u64 slide { 0 }; // How far from where it was linked.
    u64 size { 0 }; // From the lowest to the end of the highest segment, page-aligned.
    u64 entry { 0 };
};

// Copies the kernel's loadable segments into one physically contiguous
// block, laid out exactly as their virtual addresses are, with those moved up
// by `slide` (kernel address space layout randomization). Uninitialized data
// (memory size beyond file size) ends up zeroed.
ErrorOr<LoadedKernel> load_kernel(ELF::Image const&, UEFI::BootServices&, u64 slide);

}
