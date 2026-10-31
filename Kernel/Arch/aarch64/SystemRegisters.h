/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// Access to AArch64 system registers by name: READ_SYSTEM_REGISTER(tcr_el1).

#define READ_SYSTEM_REGISTER(name)                     \
    ({                                                 \
        u64 value_;                                    \
        asm volatile("mrs %0, " #name : "=r"(value_)); \
        value_;                                        \
    })

#define WRITE_SYSTEM_REGISTER(name, value) \
    asm volatile("msr " #name ", %0" ::"r"(static_cast<u64>(value)) : "memory")

namespace Kernel::AArch64 {

inline void instruction_barrier()
{
    asm volatile("isb" ::: "memory");
}

}
