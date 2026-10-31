/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// System call numbers, generated from Syscalls.def. Shared by the kernel
// and LibSystem.

namespace Kernel::API {

enum class Syscall : unsigned long {
#define SYSCALL(name, arguments) name,
#include <Kernel/API/Syscalls.def>
#undef SYSCALL
    Count,
};

}
