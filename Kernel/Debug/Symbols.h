/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/BootInfo.h>
#include <LibBase/Format.h>

// Kernel symbol lookup for backtraces. The symbol table comes from the
// kernel's own ELF file, which VultureBoot keeps in memory for this.

namespace Kernel::Symbols {

void initialize(API::PhysicalRange kernel_file, u64 slide);

[[nodiscard]] bool is_available();

// Writes "Kernel::panic+0x24", or "??" when the address is unknown.
void write_symbolicated(FormatSink&, FlatPtr address);

}
