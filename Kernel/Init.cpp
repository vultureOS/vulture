/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/BootInfo.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/BootArguments.h>
#include <Kernel/Console/BootScreen.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Debug/Symbols.h>
#include <Kernel/Devices/Serial/PL011.h>
#include <Kernel/Library/CxxRuntime.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace Kernel {

static StringView region_type_name(API::MemoryRegionType type)
{
    switch (type) {
    case API::MemoryRegionType::Usable:
        return "usable"_sv;
    case API::MemoryRegionType::Reserved:
        return "reserved"_sv;
    case API::MemoryRegionType::AcpiReclaimable:
        return "ACPI reclaimable"_sv;
    case API::MemoryRegionType::AcpiNvs:
        return "ACPI NVS"_sv;
    case API::MemoryRegionType::Mmio:
        return "MMIO"_sv;
    case API::MemoryRegionType::RuntimeServicesCode:
        return "UEFI runtime code"_sv;
    case API::MemoryRegionType::RuntimeServicesData:
        return "UEFI runtime data"_sv;
    case API::MemoryRegionType::KernelImage:
        return "kernel image"_sv;
    case API::MemoryRegionType::KernelFile:
        return "kernel file"_sv;
    case API::MemoryRegionType::BootArchive:
        return "boot archive"_sv;
    case API::MemoryRegionType::LoaderReclaimable:
        return "loader reclaimable"_sv;
    }
    return "unknown"_sv;
}

static FixedStringBuilder<16> human_size(u64 bytes)
{
    FixedStringBuilder<16> text;
    if (bytes >= GiB && bytes % GiB == 0)
        format_to(text, "{} GiB", bytes / GiB);
    else if (bytes >= 10 * MiB)
        format_to(text, "{} MiB", bytes / MiB);
    else
        format_to(text, "{} KiB", bytes / KiB);
    return text;
}

static void print_memory_map(API::BootInfo const& boot_info)
{
    auto const* regions = reinterpret_cast<API::MemoryRegion const*>(API::direct_map_base + boot_info.memory_map);
    dmesgln("Memory map ({} regions):", boot_info.memory_map_entries);
    u64 usable = 0;
    for (u64 i = 0; i < boot_info.memory_map_entries; ++i) {
        auto const& region = regions[i];
        auto size = human_size(region.length);
        dmesgln("  {:#018x}-{:#018x} {:>9}  {}", region.base, region.base + region.length - 1, size.view(), region_type_name(region.type));
        if (region.type == API::MemoryRegionType::Usable)
            usable += region.length;
    }
    auto usable_text = human_size(usable);
    dmesgln("Usable memory: {}", usable_text.view());
}

static void print_machine(API::BootInfo const& boot_info)
{
    if (boot_info.device_tree.size == 0) {
        dmesgln("Machine: no device tree");
        return;
    }
    ReadonlyBytes blob { reinterpret_cast<u8 const*>(API::direct_map_base + boot_info.device_tree.base), boot_info.device_tree.size };
    auto tree = DeviceTree::FlattenedDeviceTree::parse(blob);
    if (tree.is_error()) {
        dmesgln("Machine: unreadable device tree ({})", tree.error());
        return;
    }
    auto root = tree.value().find_node_by_path("/"_sv);
    if (!root.has_value())
        return;
    auto name = tree.value().find_property(root.value(), "model"_sv);
    if (!name.has_value())
        name = tree.value().find_property(root.value(), "compatible"_sv);
    dmesgln("Machine: {} (device tree, {} KiB)", name.has_value() ? name->as_string() : "unknown"_sv, boot_info.device_tree.size / KiB);
}

// "fault=<kind>" on the command line ends the boot with a deliberate
// failure, to check that crashes are reported properly.
[[gnu::noinline]] static void trigger_deliberate_fault(StringView kind)
{
    dmesgln("Triggering a deliberate fault: {}", kind);
    if (kind == "data-abort"_sv) {
        // Lower-half translation is off, so this faults at translation level 0.
        *reinterpret_cast<u32 volatile*>(0x10) = 0xdead;
    } else if (kind == "trap"_sv) {
        __builtin_trap();
    } else if (kind == "verify"_sv) {
        VERIFY(kind.is_empty());
    } else if (kind == "panic"_sv) {
        panic("deliberate panic"_sv);
    } else {
        dmesgln("Unknown fault kind; expected data-abort, trap, verify or panic");
    }
}

}

using namespace Kernel;

extern "C" [[noreturn]] void kernel_main(API::BootInfo const* boot_info)
{
    // Nothing can report a bad handoff yet; stop rather than guess.
    if (!boot_info || boot_info->magic != API::boot_info_magic || boot_info->version != API::boot_info_version || boot_info->size != sizeof(API::BootInfo))
        Arch::halt();

    run_global_constructors();
    Arch::disable_lower_half_translation();

    auto& log = kernel_log();
    log.initialize();
    if (boot_info->serial.type == API::SerialType::PL011) {
        auto& serial = boot_serial();
        serial.initialize(API::direct_map_base + boot_info->serial.physical_base);
        log.attach_serial(&serial);
    }
    Symbols::initialize(boot_info->kernel_file);

    BootArguments arguments { StringView(boot_info->command_line) };
    bool verbose = arguments.has_flag("-v"_sv);

    auto& framebuffer = boot_framebuffer();
    framebuffer.initialize(boot_info->framebuffer);
    if (framebuffer.is_present()) {
        if (verbose) {
            auto& console = boot_text_console();
            console.initialize(framebuffer);
            log.attach_console(&console);
        } else {
            BootScreen::show(framebuffer);
        }
    }
    auto show_progress = [&](u32 percent) {
        if (framebuffer.is_present() && !verbose)
            BootScreen::set_progress(framebuffer, percent);
    };

    dmesgln("vultureOS {} - vultureKernel (aarch64)", VULTURE_VERSION_STRING);
    FixedStringBuilder<160> cpu;
    Arch::describe_cpu(cpu);
    dmesgln("CPU: {}", cpu.view());
    dmesgln("Timer: {} kHz", Arch::monotonic_counter_frequency() / 1000);
    dmesgln("Boot arguments: {}", arguments.text().is_empty() ? "(none)"_sv : arguments.text());
    print_machine(*boot_info);
    show_progress(25);

    print_memory_map(*boot_info);
    show_progress(50);

    if (framebuffer.is_present())
        dmesgln("Framebuffer: {}x{} at {:#x}", framebuffer.width(), framebuffer.height(), boot_info->framebuffer.physical_base);
    else
        dmesgln("Framebuffer: none");
    dmesgln("Kernel: {} KiB image at {:#x}, symbols {}", boot_info->kernel_image.size / KiB, boot_info->kernel_image.base,
        Symbols::is_available() ? "loaded"_sv : "unavailable"_sv);
    dmesgln("Entropy from firmware: {} bytes", boot_info->entropy_size);
    show_progress(75);

    if (auto fault = arguments.value("fault"_sv); fault.has_value())
        trigger_deliberate_fault(fault.value());

    show_progress(100);
    dmesgln("vultureKernel: boot complete");
    Arch::halt();
}
