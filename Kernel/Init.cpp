/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__x86_64__)
#    include <Kernel/Arch/x86_64/APIC.h>
#    include <Kernel/Firmware/ACPI.h>
#endif
#include <Kernel/Memory/Compressor.h>
#include <Kernel/Security/Privacy.h>
#include <Kernel/Security/CodeSigning.h>
#include <Kernel/Security/Seal.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Security/DebugFault.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Memory/AddressSpace.h>
#include <Kernel/API/BootInfo.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/Power.h>
#include <Kernel/BootArguments.h>
#include <Kernel/Bus/PCI/PCI.h>
#include <Kernel/Console/BootScreen.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Devices/Audio/Audio.h>
#include <Kernel/Net/NetworkStack.h>
#include <Kernel/Devices/Graphics/Graphics.h>
#include <Kernel/Devices/Input/Input.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Debug/Symbols.h>
#include <Kernel/Devices/USB/USB.h>
#include <Kernel/Devices/Serial/BootSerial.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/FileSystem/IOBuffer.h>
#include <Kernel/FileSystem/BootArchive.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/Debug/DiskLog.h>
#include <Kernel/FileSystem/FATFS.h>
#include <Kernel/FileSystem/TmpFS.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/FileSystem/VultureFS.h>
#include <Kernel/Library/CxxRuntime.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Security/Random.h>
#include <Kernel/Security/StackProtector.h>
#include <Kernel/TTY/Terminals.h>
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
    case API::MemoryRegionType::LowMemory:
        return "low memory"_sv;
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

#if !defined(__x86_64__)
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
#endif

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

static void make_directory(StringView path)
{
    auto result = VirtualFileSystem::mkdir(path, "/"_sv, 0755);
    if (result.is_error() && result.error().code() != EEXIST)
        must("making a mount point"_sv, move(result));
}

// Mounts every FAT volume found on the disks under /Volumes, the way macOS
// shows them: the EFI System Partition (the one holding an EFI directory)
// as /Volumes/EFI, others by device name.
static void mount_volumes()
{
    bool made_volumes = false;
    for (auto& device : BlockDevice::all()) {
        auto file_system = create_fat_file_system(*device);
        if (file_system.is_error())
            continue;
        if (!made_volumes) {
            make_directory("/Volumes"_sv);
            made_volumes = true;
        }
        bool is_efi = !file_system.value()->root_inode()->lookup("EFI"_sv).is_error();
        StringBuilder path;
        path.append("/Volumes/"_sv);
        path.append(is_efi ? "EFI"_sv : device->name());
        make_directory(path.view());
        if (auto result = VirtualFileSystem::mount(file_system.release_value(), path.view()); result.is_error()) {
            dmesgln("Volumes: can't mount {} at {}: {}", device->name(), path.view(), result.error());
            continue;
        }
        dmesgln("Volumes: {} (FAT) at {}", device->name(), path.view());
    }
}

// A disk may have a system for each of several kinds of machine, which says which it's for. One that
// doesn't say is taken at its word.
static bool is_system_for_this_machine(FileSystem& file_system)
{
    RefPtr<Inode> inode = file_system.root_inode();
    StringView const components[] = { "System"_sv, "Library"_sv, "CoreServices"_sv, "Architecture"_sv };
    for (auto component : components) {
        auto next = inode->lookup(component);
        if (next.is_error())
            return true;
        inode = next.release_value();
    }
    char text[32];
    auto read = inode->read(0, sizeof(text), IOBuffer::kernel(text));
    if (read.is_error())
        return true;
    StringView const name = Arch::architecture_name();
    return read.value() >= name.length() && StringView(text, name.length()) == name;
}

// The root filesystem: the first vultureFS partition that mounts, or, with
// none, a tmpfs filled from the boot archive (everything then lives in
// memory and is gone at power off).
static bool mount_root_file_system(API::BootInfo const& boot_info)
{
    for (auto& device : BlockDevice::all()) {
        if (!is_vulturefs_partition(*device))
            continue;
        auto file_system = mount_vulturefs(*device);
        if (file_system.is_error()) {
            dmesgln("Root: cannot mount {}: {}", device->name(), file_system.error());
            continue;
        }
        if (!is_system_for_this_machine(*file_system.value())) {
            dmesgln("Root: {} is for another kind of machine", device->name());
            continue;
        }
        VirtualFileSystem::mount_root(file_system.release_value());
        dmesgln("Root: vultureFS on {}", device->name());
        return true;
    }
    VirtualFileSystem::mount_root(must("root filesystem"_sv, TmpFS::create()));
    if (boot_info.boot_archive.size == 0)
        return false;
    ReadonlyBytes archive { physical_to_virtual(PhysicalAddress(boot_info.boot_archive.base)), boot_info.boot_archive.size };
    auto unpacked = must("unpacking the boot archive"_sv, unpack_boot_archive(archive));
    dmesgln("Root: tmpfs with {} files ({} KiB) and {} directories from the boot archive", unpacked.files, unpacked.bytes / KiB, unpacked.directories);
    return true;
}

// Every few seconds, everything changed is written and committed, so a
// crash loses at most that much.
static void start_sync_thread()
{
    auto thread = must("sync thread"_sv, Thread::create("sync"_sv, [](void*) {
        for (;;) {
            (void)Thread::sleep_for_microseconds(5'000'000);
            if (auto result = VirtualFileSystem::sync_all(); result.is_error())
                dmesgln("sync: {}", result.error());
        } }, nullptr, { .qos = QoS::Utility }));
    thread->start();
}

// Builds the file tree (the root, devfs at /dev, a fresh tmpfs at /tmp,
// FAT volumes under /Volumes), then starts /sbin/vlaunchd as process 1.
static void start_userland(API::BootInfo const& boot_info, BootArguments const& arguments)
{
    // devfs comes first: disks add themselves to it as they are found.
    auto devices = must("devfs"_sv, DevFS::create());
    must("devices"_sv, create_basic_devices());
    must("terminals"_sv, create_terminals());
    initialize_graphics();
    initialize_input();
    initialize_audio();
    Net::NetworkStack::initialize();
    Privacy::initialize();
    Compressor::initialize();
    initialize_storage();
    // Keyboards, and also disks: so after those on the machine, and before looking for a root.
    USB::initialize();
    if (!mount_root_file_system(boot_info)) {
        dmesgln("Userland: no root disk and no boot archive, so no init");
        return;
    }
    make_directory("/dev"_sv);
    make_directory("/tmp"_sv);
    must("devfs"_sv, VirtualFileSystem::mount(move(devices), "/dev"_sv));
    mount_volumes();
    DiskLog::start();
    Seal::initialize(!arguments.has_flag("noseal"_sv));
    CodeSigning::initialize(!arguments.has_flag("nocodesign"_sv));
    auto temporary = must("tmpfs"_sv, TmpFS::create());
    // Anyone may make files in /tmp; only their owners may remove them.
    must("/tmp"_sv, temporary->root_inode()->set_permissions(01777));
    must("/tmp"_sv, VirtualFileSystem::mount(move(temporary), "/tmp"_sv));
    start_sync_thread();

    // The boot screen gives way to the text console, /dev/console's screen.
    auto& framebuffer = boot_framebuffer();
    auto& console = boot_text_console();
    if (framebuffer.is_present() && !console.is_ready()) {
        console.initialize(framebuffer);
        kernel_log().attach_console(&console);
    }

    // Boot flags that make vlaunchd run tests: "utest" the userland tests,
    // "crashtest" and "persisttest" the storage tests.
    // ("utest=NAME" runs that one alone.)
    StringView init_arguments[4] = { "/sbin/vlaunchd"_sv, {}, {}, {} };
    size_t argument_count = 1;
    if (arguments.has_flag("utest"_sv)) {
        init_arguments[argument_count++] = "--test"_sv;
    } else if (auto only_test = arguments.value("utest"_sv); only_test.has_value()) {
        init_arguments[argument_count++] = "--test"_sv;
        init_arguments[argument_count++] = only_test.value();
    } else if (arguments.has_flag("crashtest"_sv))
        init_arguments[argument_count++] = "--crash-test"_sv;
    else if (arguments.has_flag("persisttest"_sv))
        init_arguments[argument_count++] = "--persist-test"_sv;
    else if (arguments.has_flag("uitest"_sv))
        init_arguments[argument_count++] = "--ui-test"_sv;
    else if (arguments.has_flag("uigallery"_sv))
        init_arguments[argument_count++] = "--ui-gallery"_sv;
    else if (arguments.has_flag("uidesktop"_sv))
        init_arguments[argument_count++] = "--ui-desktop"_sv;
    else if (arguments.has_flag("soundtest"_sv))
        init_arguments[argument_count++] = "--sound-test"_sv;
    else if (arguments.has_flag("nettest"_sv))
        init_arguments[argument_count++] = "--network-test"_sv;
    else if (arguments.has_flag("netguitest"_sv))
        init_arguments[argument_count++] = "--network-gui-test"_sv;
    else if (!arguments.has_flag("console"_sv))
        // The desktop, unless asked for the text console: what an image must do, having
        // no boot arguments. (vlaunchd falls back on the console if there is no screen.)
        init_arguments[argument_count++] = "--gui"_sv;
    if (arguments.has_flag("debug"_sv)) {
        Process::set_tracing(true);
        init_arguments[argument_count++] = "--debug"_sv;
    }
    (void)must("starting init"_sv, Process::create_init("/sbin/vlaunchd"_sv, Span<StringView const>(init_arguments, argument_count)));
}

static void initialize_interrupts_and_time(DeviceTree::FlattenedDeviceTree const& tree)
{
    must("interrupt controller"_sv, Arch::initialize_interrupt_controller(tree));
    must("timer"_sv, Time::initialize(tree));
    Time::initialize_wall_clock(tree);
    Processor::current().initialize_this_processor();
    Arch::enable_interrupts();

    FixedStringBuilder<160> description;
    Arch::describe_interrupt_controller(description);
    dmesgln("Interrupts: {}", description.view());
    dmesgln("Timer: {} kHz, tickless", Time::ticks_per_second() / 1000);
}

// For fault=stack-smash. No inlining or the like: it should look like the real thing.
[[gnu::noinline]] static void smash_my_stack(size_t count)
{
    char array[16];
    char volatile* volatile pointer = array;
    for (size_t i = 0; i < count; ++i)
        pointer[i] = 'A';
}

// For fault=pac-return: what overwriting a return address does, without being able to sign it.
[[gnu::noinline]] static void return_to_a_changed_address()
{
    dmesgln("Changing this function's return address");
    auto* frame = static_cast<u64*>(__builtin_frame_address(0));
    frame[1] ^= 0x10;
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
    } else if (kind == "stack-smash"_sv) {
        // Past the end of an array on this frame, up over what the compiler put there.
        smash_my_stack(64);
#if VULTURE_KERNEL_SANITIZERS
    } else if (kind == "kasan-overflow"_sv) {
        auto* volatile bytes = static_cast<u8*>(kmalloc(40));
        bytes[40] = 1;
    } else if (kind == "kasan-overflow-partial"_sv) {
        // Inside the last eight bytes of it, and past the end all the same.
        auto* volatile bytes = static_cast<u8*>(kmalloc(37));
        bytes[36] = 1;
        dmesgln("The last byte was fine");
        bytes[37] = 1;
    } else if (kind == "kasan-use-after-free"_sv) {
        auto* volatile bytes = static_cast<u8*>(kmalloc(40));
        kfree(const_cast<u8*>(bytes));
        dmesgln("Read after free: {}", static_cast<u32>(bytes[0]));
    } else if (kind == "kasan-double-free"_sv) {
        void* pointer = kmalloc(40);
        kfree(pointer);
        kfree(pointer);
    } else if (kind == "kasan-memcpy"_sv) {
        auto* volatile bytes = static_cast<u8*>(kmalloc(16));
        char volatile source[32] = {};
        __builtin_memcpy(const_cast<u8*>(bytes), const_cast<char*>(source), 32);
    } else if (kind == "ubsan-overflow"_sv) {
        int volatile large = 2147483647;
        dmesgln("Overflow: {}", large + 1);
    } else if (kind == "ubsan-shift"_sv) {
        int volatile by = 40;
        dmesgln("Shift: {}", 1 << by);
    } else if (kind == "ubsan-bounds"_sv) {
        int array[4] = {};
        // (Four is one past the end, which may be pointed at; so one further.)
        int volatile index = 5;
        dmesgln("Past the end: {}", array[index]);
#endif
    } else if (kind == "pac-return"_sv) {
        return_to_a_changed_address();
    } else if (kind == "bti-branch"_sv) {
        // Into the middle of a function: not somewhere anyone should be able to jump.
        auto* middle = reinterpret_cast<u8 const*>(&smash_my_stack) + 12;
        reinterpret_cast<void (*)(size_t)>(const_cast<u8*>(middle))(0);
    } else if (kind == "text-write"_sv) {
        // Code isn't writable: W^X for the kernel.
        *reinterpret_cast<u32 volatile*>(&trigger_deliberate_fault) = 0;
    } else if (kind == "rodata-write"_sv) {
        static char const constant[] = "constants stay constant";
        *const_cast<char volatile*>(&constant[0]) = 'X';
    } else if (kind == "data-execute"_sv) {
#if defined(__x86_64__)
        static u32 data[4] = { 0x90909090, 0x90909090, 0x000000c3, 0 };
#else
        static u32 data[4] = { 0xd503201f, 0xd503201f, 0xd65f03c0, 0 };
#endif
        reinterpret_cast<void (*)()>(data)();
    } else {
        dmesgln("Unknown fault kind");
    }
}

}

using namespace Kernel;

// The kernel's copy: the loader's is in memory the kernel reclaims.
static API::BootInfo s_boot_info;

extern "C" [[noreturn, gnu::no_stack_protector]] void kernel_main(API::BootInfo const* loader_boot_info)
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
#if defined(__x86_64__)
    if (boot_info.serial.type == API::SerialType::UART16550) {
        auto& serial = boot_serial();
        serial.initialize(boot_info.serial.physical_base);
        log.attach_serial(&serial);
    }
#else
    if (boot_info.serial.type == API::SerialType::PL011) {
        auto& serial = boot_serial();
        serial.initialize(API::direct_map_base + boot_info.serial.physical_base);
        log.attach_serial(&serial);
    }
#endif
    Symbols::initialize(boot_info.kernel_file, boot_info.kernel_slide);

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

    dmesgln("vultureOS {} - vultureKernel ({})", VULTURE_VERSION_STRING, Arch::architecture_name());
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
#if defined(__aarch64__)
    set_sd_controller_clock(boot_info.storage_clock_hz);
#endif
    initialize_random({ boot_info.entropy, min<size_t>(boot_info.entropy_size, sizeof(boot_info.entropy)) });
    initialize_stack_protector();
    show_progress(20);

    initialize_memory(boot_info, framebuffer);
    show_progress(40);

#if defined(__x86_64__)
    // A PC describes itself with ACPI's tables, and has no tree. The functions below take one for
    // the architecture that does.
    if (!ACPI::initialize(PhysicalAddress(boot_info.acpi_rsdp)))
        panic("no ACPI tables: vultureKernel needs them to find its hardware"_sv);
    Arch::set_low_memory(PhysicalAddress(boot_info.low_memory));
    auto const tree = DeviceTree::FlattenedDeviceTree::empty();
#else
    auto& mm = memory_manager();
    if (mm.device_tree().is_empty())
        panic("no device tree: vultureKernel needs one to find its hardware"_sv);
    auto tree = must("device tree"_sv, DeviceTree::FlattenedDeviceTree::parse(mm.device_tree()));
    print_machine(tree);
#endif

    initialize_interrupts_and_time(tree);
    if (auto result = Arch::initialize_power_management(tree); result.is_error())
        dmesgln("Power management: unavailable ({})", result.error());
    FixedStringBuilder<64> power;
    Arch::describe_power_management(power);
    dmesgln("Power management: {}", power.view());
    if (auto result = PCI::initialize(tree); result.is_error()) {
        dmesgln("PCI: unavailable ({})", result.error());
    } else {
        FixedStringBuilder<96> pci;
        PCI::describe(pci);
        dmesgln("PCI: {}", pci.view());
    }

    {
        FixedStringBuilder<96> protections;
        Arch::describe_hardening_features(protections);
        dmesgln("Protections: {}", protections.view());
    }

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
    // See "debug.fault" in sys_sysctl.
    if (auto fault = arguments.value("debugfault"_sv); fault.has_value())
        set_debug_fault(fault.value());

    show_progress(100);
    dmesgln("vultureKernel: boot complete");
    start_userland(boot_info, arguments);
    // From here on, the processors run user programs, or idle.
    Thread::exit();
}
