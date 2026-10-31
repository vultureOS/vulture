/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Format.h>

namespace DeviceTree {
class FlattenedDeviceTree;
}

namespace Kernel {

// Interrupts processors send each other.
enum class IPI : u8 {
    Wakeup, // Look for new work.
    Stop, // Halt (a panic is in progress).
};

constexpr u8 ipi_count = 2;

}

namespace Kernel::Arch {

// Finds the interrupt controller in the device tree and resets its shared
// part. Once, on the boot processor.
ErrorOr<void> initialize_interrupt_controller(DeviceTree::FlattenedDeviceTree const&);

// Sets up the current processor's part of the controller.
ErrorOr<void> initialize_interrupt_controller_on_this_processor();

// Per-processor interrupts (such as the timer's) are enabled for the
// current processor only.
void enable_interrupt(u32 interrupt);
void disable_interrupt(u32 interrupt);

void send_ipi(u64 target_hardware_id, IPI);
void send_ipi_to_others(IPI);

void describe_interrupt_controller(FormatSink&);

}
