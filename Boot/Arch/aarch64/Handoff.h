/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>

namespace Boot::AArch64 {

// Checks that the CPU state is what the handoff expects: running at EL1,
// with 4 KiB translation granules available.
ErrorOr<void> check_cpu();

// ID_AA64MMFR0_EL1.PARange: the CPU's physical address size.
[[nodiscard]] u64 physical_address_size();

// Register values the kernel starts with. The layout is read by the
// trampoline, so do not reorder.
struct HandoffState {
    u64 mair;
    u64 tcr;
    u64 ttbr0;
    u64 ttbr1;
    u64 sctlr;
    u64 entry;
    u64 boot_info;
};

[[nodiscard]] HandoffState make_handoff_state(u64 ttbr0, u64 ttbr1, u64 entry, u64 boot_info);

// Writes a range back from the data cache to memory (the point of coherency).
void clean_data_cache(u64 start, u64 size);

// Makes code that was copied as data visible to instruction fetch.
void synchronize_instruction_cache(u64 start, u64 size);

// Where the switch-over code lives; it must be identity-mapped in TTBR0.
[[nodiscard]] u64 trampoline_address();

// Masks interrupts, installs the new translation tables and jumps to the
// kernel with x0 = state.boot_info. Never returns.
[[noreturn]] void enter_kernel(HandoffState const& state);

}
