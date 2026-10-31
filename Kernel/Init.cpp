/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/BootInfo.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/Power.h>
#include <Kernel/BootArguments.h>
#include <Kernel/Console/BootScreen.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Debug/Symbols.h>
#include <Kernel/Devices/Serial/PL011.h>
#include <Kernel/FileSystem/BootArchive.h>
#include <Kernel/Library/CxxRuntime.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

extern "C" u8 boot_stack_bottom[];
extern "C" u8 boot_stack_top[];

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

static void print_machine(DeviceTree::FlattenedDeviceTree const& tree)
{
    auto root = tree.find_node_by_path("/"_sv);
    if (!root.has_value())
        return;
    auto name = tree.find_property(root.value(), "model"_sv);
    if (!name.has_value())
        name = tree.find_property(root.value(), "compatible"_sv);
    dmesgln("Machine: {} (device tree, {} KiB)", name.has_value() ? name->as_string() : "unknown"_sv, tree.total_size() / KiB);
}

template<typename T>
static T must(StringView what, ErrorOr<T>&& result)
{
    if (result.is_error()) [[unlikely]] {
        FixedStringBuilder<160> reason;
        format_to(reason, "{}: {}", what, result.error());
        panic(reason.view());
    }
    return result.release_value();
}

// A framebuffer inside a memory map region (as ramfb's is) is RAM and is
// mapped cacheable; one outside it is device memory.
static Arch::MemoryType framebuffer_memory_type(API::BootInfo const& boot_info)
{
    auto const* regions = reinterpret_cast<API::MemoryRegion const*>(API::direct_map_base + boot_info.memory_map);
    u64 address = boot_info.framebuffer.physical_base;
    for (u64 i = 0; i < boot_info.memory_map_entries; ++i) {
        if (address >= regions[i].base && address - regions[i].base < regions[i].length && regions[i].type != API::MemoryRegionType::Mmio)
            return Arch::MemoryType::Normal;
    }
    return Arch::MemoryType::NormalUncached;
}

// Replaces the loader's page tables with the kernel's own and takes over
// the loader's memory.
static void initialize_memory(API::BootInfo const& boot_info, Framebuffer& framebuffer)
{
    auto& mm = memory_manager();
    must("memory"_sv, mm.initialize(boot_info));

    // Devices already in use must be mapped in the new tables before the switch.
    FlatPtr serial_registers = 0;
    if (boot_info.serial.type == API::SerialType::PL011)
        serial_registers = must("mapping the serial port"_sv, mm.map_mmio(PhysicalAddress(boot_info.serial.physical_base), page_size));
    FlatPtr pixels = 0;
    if (framebuffer.is_present())
        pixels = must("mapping the framebuffer"_sv, mm.map_mmio(PhysicalAddress(boot_info.framebuffer.physical_base), boot_info.framebuffer.size, framebuffer_memory_type(boot_info)));

    // Nothing may log from the switch until the devices are relocated: their
    // old addresses vanish with the loader's tables.
    mm.activate_kernel_page_table();
    if (serial_registers)
        boot_serial().relocate(serial_registers);
    if (pixels)
        framebuffer.relocate(pixels);

    mm.reclaim_boot_memory();
    must("user address spaces"_sv, Arch::initialize_user_translation());
    auto statistics = physical_memory().statistics();
    auto free = human_size(statistics.free_pages * page_size);
    auto managed = human_size(statistics.managed_pages * page_size);
    auto reclaimed = human_size(mm.reclaimed_bytes());
    dmesgln("Memory: {} free of {} in {} zone{}; {} reclaimed from the loader", free.view(), managed.view(), statistics.zone_count,
        statistics.zone_count == 1 ? ""_sv : "s"_sv, reclaimed.view());
    dmesgln("Memory: kernel page tables use {} KiB", mm.kernel_page_table().table_page_count() * page_size / KiB);
}

// Mounts the boot archive as / and starts /sbin/init as process 1.
static void start_userland(API::BootInfo const& boot_info, BootArguments const& arguments)
{
    if (boot_info.boot_archive.size == 0) {
        dmesgln("Userland: no boot archive, so no init");
        return;
    }
    ReadonlyBytes archive { physical_to_virtual(PhysicalAddress(boot_info.boot_archive.base)), boot_info.boot_archive.size };
    must("boot archive"_sv, BootArchive::initialize(archive));
    dmesgln("Boot archive: {} entries in {} KiB, mounted as /", BootArchive::entry_count(), archive.size() / KiB);

    // "utest" runs the userland tests.
    StringView const plain[] = { "/sbin/init"_sv };
    StringView const testing[] = { "/sbin/init"_sv, "--test"_sv };
    bool test = arguments.has_flag("utest"_sv);
    (void)must("starting init"_sv, Process::create_init("/sbin/init"_sv, test ? Span<StringView const>(testing) : Span<StringView const>(plain)));
}

static void initialize_interrupts_and_time(DeviceTree::FlattenedDeviceTree const& tree)
{
    must("interrupt controller"_sv, Arch::initialize_interrupt_controller(tree));
    must("timer"_sv, Time::initialize(tree));
    Processor::current().initialize_this_processor();
    Arch::enable_interrupts();

    FixedStringBuilder<160> description;
    Arch::describe_interrupt_controller(description);
    dmesgln("Interrupts: {}", description.view());
    dmesgln("Timer: {} kHz, tickless", Time::ticks_per_second() / 1000);
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
    } else if (kind == "interrupt"_sv) {
        // A panic in interrupt context: the backtrace must run through the
        // exception into the code that was interrupted.
        static Timer timer;
        Processor::current().timer_queue().add(timer, Time::now() + Time::ticks_from_milliseconds(1), [](Timer&) {
            panic("deliberate panic in a timer interrupt"_sv);
        });
        Time::wait_until(Time::now() + Time::ticks_from_milliseconds(100));
    } else if (kind == "processor"_sv) {
        // A panic on a secondary processor, which must stop all the others.
        Processor::run_on_all([](void*) {
            if (Processor::current().id() == 1)
                panic("deliberate panic on processor 1"_sv);
        },
            nullptr);
    } else {
        dmesgln("Unknown fault kind; expected data-abort, trap, verify, panic, interrupt or processor");
    }
}

}

using namespace Kernel;

// The kernel's copy: the loader's is in memory the kernel reclaims.
static API::BootInfo s_boot_info;

extern "C" [[noreturn]] void kernel_main(API::BootInfo const* loader_boot_info)
{
    // Nothing can report a bad handoff yet; stop rather than guess.
    if (!loader_boot_info || loader_boot_info->magic != API::boot_info_magic || loader_boot_info->version != API::boot_info_version || loader_boot_info->size != sizeof(API::BootInfo))
        Arch::halt();
    __builtin_memcpy(&s_boot_info, loader_boot_info, sizeof(s_boot_info));
    auto const& boot_info = s_boot_info;

    run_global_constructors();
    Arch::disable_lower_half_translation();
    Processor::initialize_boot_processor(reinterpret_cast<FlatPtr>(boot_stack_bottom), reinterpret_cast<FlatPtr>(boot_stack_top));

    auto& log = kernel_log();
    log.initialize();
    if (boot_info.serial.type == API::SerialType::PL011) {
        auto& serial = boot_serial();
        serial.initialize(API::direct_map_base + boot_info.serial.physical_base);
        log.attach_serial(&serial);
    }
    Symbols::initialize(boot_info.kernel_file);

    BootArguments arguments { StringView(boot_info.command_line) };
    bool verbose = arguments.has_flag("-v"_sv);

    auto& framebuffer = boot_framebuffer();
    framebuffer.initialize(boot_info.framebuffer);
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
    dmesgln("Boot arguments: {}", arguments.text().is_empty() ? "(none)"_sv : arguments.text());
    print_memory_map(boot_info);
    if (framebuffer.is_present())
        dmesgln("Framebuffer: {}x{} at {:#x}", framebuffer.width(), framebuffer.height(), boot_info.framebuffer.physical_base);
    else
        dmesgln("Framebuffer: none");
    dmesgln("Kernel: {} KiB image at {:#x}, symbols {}", boot_info.kernel_image.size / KiB, boot_info.kernel_image.base,
        Symbols::is_available() ? "loaded"_sv : "unavailable"_sv);
    dmesgln("Entropy from firmware: {} bytes", boot_info.entropy_size);
    show_progress(20);

    initialize_memory(boot_info, framebuffer);
    show_progress(40);

    auto& mm = memory_manager();
    if (mm.device_tree().is_empty())
        panic("no device tree: vultureKernel needs one to find its hardware"_sv);
    auto tree = must("device tree"_sv, DeviceTree::FlattenedDeviceTree::parse(mm.device_tree()));
    print_machine(tree);

    initialize_interrupts_and_time(tree);
    if (auto result = Arch::initialize_power_management(tree); result.is_error())
        dmesgln("Power management: unavailable ({})", result.error());
    FixedStringBuilder<64> power;
    Arch::describe_power_management(power);
    dmesgln("Power management: {}", power.view());
    // From here on, this code is the "boot" thread.
    Scheduler::initialize();
    dmesgln("Scheduler: {} QoS classes, {} ms time slices, idle processors take work", qos_class_count, Scheduler::time_slice_microseconds / 1000);
    show_progress(60);

    u64 smp_start = Time::now();
    Processor::start_secondary_processors(tree);
    dmesgln("SMP: {} of {} processors online in {} us", Processor::online_count(), Processor::listed_count(),
        Time::microseconds_from_ticks(Time::now() - smp_start));
    show_progress(80);

    if (arguments.has_flag("ktest"_sv) || arguments.value("ktest"_sv).has_value()) {
        u64 seconds = 2;
        if (auto text = arguments.value("ktest-seconds"_sv); text.has_value())
            seconds = text->to_number<u64>().value_or(seconds);
        Tests::run(arguments.value("ktest"_sv).value_or(""_sv), seconds);
    }

    if (auto fault = arguments.value("fault"_sv); fault.has_value())
        trigger_deliberate_fault(fault.value());

    show_progress(100);
    dmesgln("vultureKernel: boot complete");
    start_userland(boot_info, arguments);
    // From here on, the processors run user programs, or idle.
    Thread::exit();
}
