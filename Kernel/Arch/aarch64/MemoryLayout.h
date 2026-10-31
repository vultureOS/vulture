/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/BootInfo.h>

// The kernel half of the address space (TTBR1, 48-bit):
//
//   0xffff'0000'0000'0000  direct map of physical RAM        128 TiB
//   0xffff'8000'0000'0000  MMIO window (device mappings)       1 TiB
//   0xffff'8100'0000'0000  kernel stacks, with guard gaps      1 TiB
//   0xffff'ffff'8000'0000  the kernel image                    2 GiB

namespace Kernel::MemoryLayout {

constexpr u64 TiB = 1ull << 40;

constexpr FlatPtr direct_map_base = API::direct_map_base;
constexpr u64 direct_map_size = 128 * TiB;

constexpr FlatPtr mmio_base = 0xffff'8000'0000'0000;
constexpr u64 mmio_size = 1 * TiB;

constexpr FlatPtr kernel_stacks_base = 0xffff'8100'0000'0000;
// Each stack sits at the top of its slot; the unmapped rest of the slot
// below it turns an overflow into a fault instead of silent corruption.
constexpr u64 kernel_stack_slot_size = 128 * KiB;
constexpr u64 kernel_stack_size = 64 * KiB;
constexpr size_t kernel_stack_slots = 4096;

constexpr FlatPtr kernel_image_base = 0xffff'ffff'8000'0000;

static_assert(direct_map_base + direct_map_size <= mmio_base);
static_assert(mmio_base + mmio_size <= kernel_stacks_base);

}
