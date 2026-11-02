/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>

namespace Boot::X86_64 {

// Checks that the CPU has what the kernel can't do without.
ErrorOr<void> check_cpu();

struct HandoffState {
    u64 tables;
    u64 entry;
    u64 boot_info;
};

// Where the switch-over code lives; it must be mapped at its own address in the new tables.
[[nodiscard]] u64 trampoline_address();

// Masks interrupts, installs the new tables and jumps to the kernel with rdi = state.boot_info. Never
// returns.
[[noreturn]] void enter_kernel(HandoffState const& state);

}
