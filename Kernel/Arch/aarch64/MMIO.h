/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// Device register access with single plain load/store instructions. A
// hypervisor emulating a device must decode the faulting instruction, which
// it can only do for these (not for pairs or writeback forms the compiler
// might pick for a volatile access).

namespace Kernel::AArch64 {

inline u32 mmio_read32(FlatPtr address)
{
    u32 value;
    asm volatile("ldr %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}

inline void mmio_write32(FlatPtr address, u32 value)
{
    asm volatile("str %w0, [%1]" ::"rZ"(value), "r"(address) : "memory");
}

inline u64 mmio_read64(FlatPtr address)
{
    u64 value;
    asm volatile("ldr %0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}

inline void mmio_write64(FlatPtr address, u64 value)
{
    asm volatile("str %x0, [%1]" ::"rZ"(value), "r"(address) : "memory");
}

}
