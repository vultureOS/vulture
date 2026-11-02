/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/InterruptController.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace Kernel::AArch64 {

// From the IRQ exception vector: acknowledges, dispatches and completes
// every pending interrupt.
void handle_irq();

}

// The two kinds of interrupt controller there are: Arm's GICv3, as on QEMU's virt machine and on servers, and
// the GICv2 of the Raspberry Pi. (See GIC.cpp for which one is used.)

namespace Kernel::AArch64::GICv3 {

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const&, DeviceTree::Node const&);
ErrorOr<void> initialize_on_this_processor();
void enable_interrupt(u32);
void disable_interrupt(u32);
void send_ipi(u64 target_hardware_id, IPI);
void send_ipi_to_others(IPI);
void describe(FormatSink&);
void handle_irq();

}

namespace Kernel::AArch64::GICv2 {

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const&, DeviceTree::Node const&);
ErrorOr<void> initialize_on_this_processor();
void enable_interrupt(u32);
void disable_interrupt(u32);
void send_ipi(u64 target_hardware_id, IPI);
void send_ipi_to_others(IPI);
void describe(FormatSink&);
void handle_irq();

}
