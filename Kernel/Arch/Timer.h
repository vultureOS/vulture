/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>

namespace DeviceTree {
class FlattenedDeviceTree;
}

// Each processor's one-shot timer, counting in monotonic_counter() ticks.

namespace Kernel::Arch {

// Finds the timer and returns its (per-processor) interrupt number.
ErrorOr<u32> initialize_timer(DeviceTree::FlattenedDeviceTree const&);

// Stops this processor's timer and enables its interrupt.
void initialize_timer_on_this_processor();

// Raises the timer interrupt once the counter reaches `deadline`
// (immediately if it already has).
void set_timer_deadline(u64 deadline);
void stop_timer();

}
