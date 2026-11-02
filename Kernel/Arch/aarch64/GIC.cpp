/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/aarch64/GIC.h>

// Whichever interrupt controller the device tree names.

namespace Kernel::Arch {

namespace {

enum class Version {
    None,
    Two,
    Three,
};

Version s_version = Version::None;

}

ErrorOr<void> initialize_interrupt_controller(DeviceTree::FlattenedDeviceTree const& device_tree)
{
    if (auto node = device_tree.find_compatible_node("arm,gic-v3"_sv); node.has_value()) {
        TRY(AArch64::GICv3::initialize(device_tree, node.value()));
        s_version = Version::Three;
        return {};
    }
    static constexpr StringView version_two_names[] = { "arm,gic-400"_sv, "arm,cortex-a15-gic"_sv, "arm,cortex-a7-gic"_sv };
    for (auto name : version_two_names) {
        auto node = device_tree.find_compatible_node(name);
        if (!node.has_value())
            continue;
        TRY(AArch64::GICv2::initialize(device_tree, node.value()));
        s_version = Version::Two;
        return {};
    }
    return Error::from_string_literal("no interrupt controller in the device tree");
}

ErrorOr<void> initialize_interrupt_controller_on_this_processor()
{
    return s_version == Version::Two ? AArch64::GICv2::initialize_on_this_processor() : AArch64::GICv3::initialize_on_this_processor();
}

void enable_interrupt(u32 interrupt)
{
    if (s_version == Version::Two)
        AArch64::GICv2::enable_interrupt(interrupt);
    else
        AArch64::GICv3::enable_interrupt(interrupt);
}

void disable_interrupt(u32 interrupt)
{
    if (s_version == Version::Two)
        AArch64::GICv2::disable_interrupt(interrupt);
    else
        AArch64::GICv3::disable_interrupt(interrupt);
}

void send_ipi(u64 target_hardware_id, IPI ipi)
{
    if (s_version == Version::Two)
        AArch64::GICv2::send_ipi(target_hardware_id, ipi);
    else if (s_version == Version::Three)
        AArch64::GICv3::send_ipi(target_hardware_id, ipi);
}

void send_ipi_to_others(IPI ipi)
{
    if (s_version == Version::Two)
        AArch64::GICv2::send_ipi_to_others(ipi);
    else if (s_version == Version::Three)
        AArch64::GICv3::send_ipi_to_others(ipi);
}

void describe_interrupt_controller(FormatSink& sink)
{
    if (s_version == Version::Two)
        AArch64::GICv2::describe(sink);
    else
        AArch64::GICv3::describe(sink);
}

}

namespace Kernel::AArch64 {

void handle_irq()
{
    if (Arch::s_version == Arch::Version::Two)
        GICv2::handle_irq();
    else
        GICv3::handle_irq();
}

}
