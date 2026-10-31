/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/Timer.h>
#include <Kernel/Arch/aarch64/SystemRegisters.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

// The Arm generic timer's EL1 virtual timer (CNTV_*). It compares against
// the virtual counter, the same one monotonic_counter() reads.

namespace Kernel::Arch {

static constexpr u64 timer_enable = 1 << 0;
static constexpr u32 first_ppi = 16;
// "interrupts" lists the secure physical, non-secure physical, virtual and
// hypervisor timers, in that order, three cells each (type, number, flags).
static constexpr size_t virtual_timer_index = 2;
static constexpr size_t cells_per_interrupt = 3;
static constexpr u32 interrupt_type_ppi = 1;

static u32 s_interrupt = 0;

ErrorOr<u32> initialize_timer(DeviceTree::FlattenedDeviceTree const& device_tree)
{
    auto node = device_tree.find_compatible_node("arm,armv8-timer"_sv);
    if (!node.has_value())
        return Error::from_string_literal("no Arm generic timer in the device tree");
    auto interrupts = device_tree.find_property(node.value(), "interrupts"_sv);
    if (!interrupts.has_value())
        return Error::from_string_literal("the timer has no interrupts");
    auto type = interrupts->read_cells(virtual_timer_index * cells_per_interrupt, 1);
    auto number = interrupts->read_cells(virtual_timer_index * cells_per_interrupt + 1, 1);
    if (!type.has_value() || !number.has_value() || type.value() != interrupt_type_ppi)
        return Error::from_string_literal("the virtual timer interrupt is not described");
    s_interrupt = first_ppi + static_cast<u32>(number.value());
    return s_interrupt;
}

void initialize_timer_on_this_processor()
{
    stop_timer();
    enable_interrupt(s_interrupt);
}

void set_timer_deadline(u64 deadline)
{
    WRITE_SYSTEM_REGISTER(cntv_cval_el0, deadline);
    WRITE_SYSTEM_REGISTER(cntv_ctl_el0, timer_enable);
    AArch64::instruction_barrier();
}

void stop_timer()
{
    // A disabled timer stops asserting its interrupt.
    WRITE_SYSTEM_REGISTER(cntv_ctl_el0, 0);
    AArch64::instruction_barrier();
}

}
