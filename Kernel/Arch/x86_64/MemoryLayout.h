/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/BootInfo.h>

// User space (the lower half, 47 bits), per process:
//
//   0x0000'0000'0001'0000  lowest mappable address (below: null pointer guard)
//   0x0000'0000'0040'0000  programs linked for a fixed address, if any
//   0x0000'0040'0000'0000  position-independent programs: somewhere in 256 GiB
//   0x0000'1000'0000'0000  mmap() places mappings upwards from somewhere in 1 TiB
//   0x0000'7e00'0000'0000  the dynamic linker: somewhere in 64 GiB
//   0x0000'7fff'ffff'0000  the main thread's stack (8 MiB, grows down), up to 256 MiB lower
//   0x0000'7fff'ffff'f000  end of user space (the last page can't be returned to safely)
//
// The kernel half of the address space:
//
//   0xffff'8000'0000'0000  direct map of physical RAM         64 TiB
//   0xffff'c000'0000'0000  MMIO window (device mappings)       1 TiB
//   0xffff'c100'0000'0000  kernel stacks, with guard gaps      1 TiB
//   0xffff'ffff'8000'0000  the kernel image                    2 GiB

namespace Kernel::MemoryLayout {

constexpr u64 TiB = 1ull << 40;

constexpr FlatPtr direct_map_base = API::direct_map_base;
constexpr u64 direct_map_size = 64 * TiB;

constexpr FlatPtr mmio_base = 0xffff'c000'0000'0000;
constexpr u64 mmio_size = 1 * TiB;

constexpr FlatPtr kernel_stacks_base = 0xffff'c100'0000'0000;
// Each stack sits at the top of its slot; the unmapped rest of the slot
// below it turns an overflow into a fault instead of silent corruption.
constexpr u64 kernel_stack_slot_size = 128 * KiB;
constexpr u64 kernel_stack_size = 64 * KiB;
constexpr size_t kernel_stack_slots = 4096;

constexpr FlatPtr kernel_image_base = 0xffff'ffff'8000'0000;

constexpr FlatPtr user_space_start = 0x0000'0000'0001'0000;
constexpr FlatPtr user_space_end = 0x0000'7fff'ffff'f000;
// Where each thing goes in a new process: the base, plus a random number of pages below this
// many, a different one each time (address space layout randomization).
constexpr FlatPtr user_program_base = 0x0000'0040'0000'0000;
constexpr u64 user_program_random_pages = 1ull << 26;
constexpr FlatPtr user_interpreter_base = 0x0000'7e00'0000'0000;
constexpr u64 user_interpreter_random_pages = 1ull << 24;
constexpr FlatPtr user_mmap_base = 0x0000'1000'0000'0000;
constexpr u64 user_mmap_random_pages = 1ull << 28;
constexpr FlatPtr user_stack_top = 0x0000'7fff'ffff'0000;
constexpr u64 user_stack_random_pages = 1ull << 16;
constexpr u64 user_stack_size = 8 * MiB;

static_assert(direct_map_base + direct_map_size <= mmio_base);
static_assert(mmio_base + mmio_size <= kernel_stacks_base);

}
