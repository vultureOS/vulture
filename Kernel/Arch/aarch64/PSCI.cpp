/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/aarch64/MMIO.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/Power.h>
#include <Kernel/Arch/aarch64/PSCI.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace Kernel::AArch64::PSCI {

namespace {

enum class Conduit : u8 {
    None,
    HVC, // Calls the hypervisor.
    SMC, // Calls the secure monitor.
};

// SMC Calling Convention function identifiers.
constexpr u64 psci_version = 0x8400'0000;
constexpr u64 cpu_on_64 = 0xc400'0003;
constexpr u64 system_off = 0x8400'0008;
constexpr u64 system_reset = 0x8400'0009;

Conduit s_conduit = Conduit::None;
u32 s_version = 0;

}

static i64 call(u64 function, u64 argument1 = 0, u64 argument2 = 0, u64 argument3 = 0)
{
    register u64 x0 asm("x0") = function;
    register u64 x1 asm("x1") = argument1;
    register u64 x2 asm("x2") = argument2;
    register u64 x3 asm("x3") = argument3;
    // Older firmware may clobber x4-x17 (SMCCC 1.0).
    if (s_conduit == Conduit::HVC) {
        asm volatile("hvc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
            :
            : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    } else {
        asm volatile("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
            :
            : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    }
    return static_cast<i64>(x0);
}

static Error error_from_result(i64 result)
{
    switch (result) {
    case -1:
        return Error::from_string_literal("PSCI: not supported");
    case -2:
        return Error::from_string_literal("PSCI: invalid parameters");
    case -3:
        return Error::from_string_literal("PSCI: denied");
    case -4:
        return Error::from_string_literal("PSCI: already on");
    case -5:
        return Error::from_string_literal("PSCI: on pending");
    case -6:
        return Error::from_string_literal("PSCI: internal failure");
    case -7:
        return Error::from_string_literal("PSCI: not present");
    case -8:
        return Error::from_string_literal("PSCI: disabled");
    case -9:
        return Error::from_string_literal("PSCI: invalid address");
    default:
        return Error::from_string_literal("PSCI: unknown error");
    }
}

bool is_available()
{
    return s_conduit != Conduit::None;
}

ErrorOr<void> cpu_on(u64 target, u64 entry, u64 context)
{
    if (!is_available())
        return Error::from_string_literal("PSCI is not available");
    i64 result = call(cpu_on_64, target, entry, context);
    if (result != 0)
        return error_from_result(result);
    return {};
}

}

namespace Kernel::Arch {

using namespace AArch64::PSCI;

// The power manager of a Raspberry Pi, when that's what there is.
static FlatPtr s_power_manager = 0;

ErrorOr<void> initialize_power_management(DeviceTree::FlattenedDeviceTree const& device_tree)
{
    // A Raspberry Pi 4 has no such firmware: its watchdog does these things.
    static constexpr StringView power_managers[] = { "brcm,bcm2835-pm-wdt"_sv, "brcm,bcm2835-pm"_sv, "brcm,bcm2711-pm"_sv };
    for (auto compatible : power_managers) {
        auto watchdog = device_tree.find_compatible_node(compatible);
        if (!watchdog.has_value())
            continue;
        if (auto region = device_tree.reg(watchdog.value(), 0); region.has_value())
            s_power_manager = TRY(memory_manager().map_mmio(PhysicalAddress(region->address), region->size));
        break;
    }
    static constexpr StringView compatibles[] = { "arm,psci-1.0"_sv, "arm,psci-0.2"_sv, "arm,psci"_sv };
    Optional<DeviceTree::Node> node;
    for (auto compatible : compatibles) {
        node = device_tree.find_compatible_node(compatible);
        if (node.has_value())
            break;
    }
    if (!node.has_value()) {
        if (s_power_manager)
            return {};
        return Error::from_string_literal("no PSCI firmware interface in the device tree");
    }
    auto method = device_tree.find_property(node.value(), "method"_sv);
    if (!method.has_value())
        return Error::from_string_literal("PSCI has no calling method");
    if (method->as_string() == "hvc"_sv)
        s_conduit = Conduit::HVC;
    else if (method->as_string() == "smc"_sv)
        s_conduit = Conduit::SMC;
    else
        return Error::from_string_literal("unknown PSCI calling method");
    s_version = static_cast<u32>(call(psci_version));
    return {};
}

void describe_power_management(FormatSink& sink)
{
    if (!is_available()) {
        sink.write(s_power_manager ? "the Raspberry Pi's watchdog"_sv : "none"_sv);
        return;
    }
    format_to(sink, "PSCI {}.{} via {}", s_version >> 16, s_version & 0xffff, s_conduit == Conduit::HVC ? "HVC"_sv : "SMC"_sv);
}

// The watchdog, set to go off almost at once, restarts the machine. Its firmware then looks in a register that
// survives this for which partition to start from, and "63" means don't: stay off. (There's no real off.)
static void restart_by_watchdog(bool stay_off)
{
    constexpr size_t reset_control = 0x1c;
    constexpr size_t reset_status = 0x20;
    constexpr size_t watchdog_time = 0x24;
    constexpr u32 password = 0x5a00'0000;
    constexpr u32 configuration_mask = 0x30;
    constexpr u32 configuration_full_reset = 0x20;
    constexpr u32 partition_63 = 0x555;
    constexpr u32 partition_mask = 0xaaa ^ 0xfff;
    constexpr u32 almost_at_once = 10;
    FlatPtr const manager = s_power_manager;
    if (stay_off)
        AArch64::mmio_write32(manager + reset_status, password | (AArch64::mmio_read32(manager + reset_status) & ~partition_mask) | partition_63);
    AArch64::mmio_write32(manager + watchdog_time, password | almost_at_once);
    AArch64::mmio_write32(manager + reset_control, password | (AArch64::mmio_read32(manager + reset_control) & ~configuration_mask) | configuration_full_reset);
}

void power_off()
{
    if (is_available())
        call(system_off);
    else if (s_power_manager)
        restart_by_watchdog(true);
    halt();
}

void reboot()
{
    if (is_available())
        call(system_reset);
    else if (s_power_manager)
        restart_by_watchdog(false);
    halt();
}

}

namespace Kernel::Arch {

Optional<u64> read_real_time_clock(DeviceTree::FlattenedDeviceTree const& device_tree)
{
    // The Arm PL031: its data register counts seconds.
    auto node = device_tree.find_compatible_node("arm,pl031"_sv);
    if (!node.has_value())
        return {};
    auto reg = device_tree.reg(node.value(), 0);
    if (!reg.has_value())
        return {};
    auto registers = memory_manager().map_mmio(PhysicalAddress(reg->address), reg->size);
    if (registers.is_error())
        return {};
    constexpr size_t data_register = 0x00;
    return static_cast<u64>(AArch64::mmio_read32(registers.value() + data_register));
}

}
