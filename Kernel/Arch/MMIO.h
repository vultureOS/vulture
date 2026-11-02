/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Device registers, for code that isn't specific to an architecture: Arch::mmio_read32 and friends.
#if defined(__aarch64__)
#    include <Kernel/Arch/aarch64/MMIO.h>
namespace Kernel::Arch {
using AArch64::mmio_read16;
using AArch64::mmio_read32;
using AArch64::mmio_read64;
using AArch64::mmio_read8;
using AArch64::mmio_write16;
using AArch64::mmio_write32;
using AArch64::mmio_write64;
using AArch64::mmio_write8;

// For memory shared with a device. This one: what's before it is seen before what's after it.
inline void memory_barrier()
{
    asm volatile("dmb ish" ::: "memory");
}

// And this: everything before it has reached memory by the time a device is told about it.
inline void device_barrier()
{
    asm volatile("dsb sy" ::: "memory");
}
}
#elif defined(__x86_64__)
#    include <Kernel/Arch/x86_64/MMIO.h>
namespace Kernel::Arch {

// For memory shared with a device. This one: what's before it is seen before what's after it.
inline void memory_barrier()
{
    asm volatile("mfence" ::: "memory");
}

// And this: everything before it has reached memory by the time a device is told about it.
inline void device_barrier()
{
    asm volatile("mfence" ::: "memory");
}

}
#else
#    error "Unsupported architecture"
#endif
