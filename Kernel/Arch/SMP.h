/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>

namespace Kernel::Arch {

using SecondaryEntry = void (*)(void* argument);

// Starts the processor named `hardware_id`. It switches to the kernel's
// page tables, runs on `stack_top` and calls entry(argument) with
// interrupts masked and the per-processor pointer unset.
ErrorOr<void> start_processor(u64 hardware_id, FlatPtr stack_top, SecondaryEntry, void* argument);

// Releases what start_processor() needed, once every processor it started
// is running kernel code.
void finish_starting_processors();

}
