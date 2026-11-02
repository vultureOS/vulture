/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Arch/aarch64/Handoff.h>
#include <Boot/Arch/aarch64/PageTableBuilder.h>
#include <Boot/BootVolume.h>
#include <Boot/Console.h>
#include <Boot/KernelLoader.h>
#include <Boot/Memory.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

// VultureBoot: loads the kernel from the EFI System Partition, gathers what
// the firmware knows about the machine into a BootInfo, builds the kernel's
// first translation tables and hands over. See Documentation/BootProtocol.md.

namespace Boot {

namespace API = Kernel::API;
using AArch64::Access;
using AArch64::MemoryAttribute;
using AArch64::PageTableBuilder;

static constexpr char16_t kernel_path[] = u"\\System\\Library\\Kernels\\vulture";
static constexpr char16_t boot_archive_path[] = u"\\System\\Library\\Kernels\\vulture.bootarchive";
static constexpr char16_t boot_args_path[] = u"\\System\\Library\\Kernels\\boot-args";

static UEFI::SystemTable* s_system_table = nullptr;

[[noreturn]] static void power_off_after_key_press()
{
    if (auto* input = s_system_table->ConIn) {
        input->Reset(input, false);
        UEFI::UIntN index = 0;
        s_system_table->BootServices->WaitForEvent(1, &input->WaitForKey, &index);
    }
    s_system_table->RuntimeServices->ResetSystem(UEFI::ResetType::EfiResetShutdown, UEFI::StatusCode::Success, 0, nullptr);
    for (;;)
        asm volatile("wfi");
}

[[noreturn]] static void fail(StringView step, Error const& error)
{
    if (g_console) {
        g_console->set_color(UEFI::TextColor::LightRed);
        g_console->println("\nVultureBoot: {} failed: {}", step, error);
        g_console->set_color(UEFI::TextColor::LightGray);
        g_console->println("Press any key to power off.");
    }
    power_off_after_key_press();
}

template<typename T>
static T unwrap(StringView step, ErrorOr<T>&& result)
{
    if (result.is_error())
        fail(step, result.error());
    return result.release_value();
}

static void unwrap(StringView step, ErrorOr<void>&& result)
{
    if (result.is_error())
        fail(step, result.error());
}

static void print_label(Console& console, StringView label)
{
    console.set_color(UEFI::TextColor::DarkGray);
    console.print("{:<10}", label);
    console.set_color(UEFI::TextColor::LightGray);
}

static void* find_configuration_table(UEFI::Guid const& guid)
{
    for (size_t i = 0; i < s_system_table->NumberOfTableEntries; ++i) {
        if (s_system_table->ConfigurationTable[i].VendorGuid == guid)
            return s_system_table->ConfigurationTable[i].VendorTable;
    }
    return nullptr;
}

struct DisplayMode {
    UEFI::GraphicsOutputProtocol* graphics { nullptr };
    u32 number { 0 };
    u32 width { 0 };
    u32 height { 0 };
};

// Picks the largest graphics mode with a linear 32-bit framebuffer.
static Optional<DisplayMode> choose_display_mode(UEFI::BootServices& boot_services)
{
    // Every display the firmware knows of: some only offer blitting (virtio-gpu
    // does, until the kernel's driver takes it), and we need memory to draw in.
    constexpr u32 by_protocol = 2;
    UEFI::UIntN handle_count = 0;
    UEFI::Handle* handles = nullptr;
    if (UEFI::is_error(boot_services.LocateHandleBuffer(by_protocol, &UEFI::graphics_output_protocol_guid, nullptr, &handle_count, &handles)))
        return {};

    auto is_linear = [](UEFI::GraphicsPixelFormat format) {
        return format == UEFI::GraphicsPixelFormat::PixelBlueGreenRedReserved8BitPerColor
            || format == UEFI::GraphicsPixelFormat::PixelRedGreenBlueReserved8BitPerColor;
    };

    Optional<DisplayMode> best;
    for (UEFI::UIntN handle = 0; handle < handle_count; ++handle) {
        void* interface = nullptr;
        if (UEFI::is_error(boot_services.HandleProtocol(handles[handle], &UEFI::graphics_output_protocol_guid, &interface)) || !interface)
            continue;
        auto* graphics = static_cast<UEFI::GraphicsOutputProtocol*>(interface);
        for (u32 number = 0; number < graphics->Mode->MaxMode; ++number) {
            UEFI::UIntN info_size = 0;
            UEFI::GraphicsOutputModeInformation* info = nullptr;
            if (UEFI::is_error(graphics->QueryMode(graphics, number, &info_size, &info)))
                continue;
            u64 area = u64(info->HorizontalResolution) * info->VerticalResolution;
            if (is_linear(info->PixelFormat) && (!best.has_value() || area > u64(best->width) * best->height))
                best = DisplayMode { graphics, number, info->HorizontalResolution, info->VerticalResolution };
            boot_services.FreePool(info);
        }
    }
    boot_services.FreePool(handles);
    return best;
}

// Switches to the chosen mode and describes its framebuffer. Runs after the
// loader's last screen output: the firmware's text console expects its own
// mode, and resetting it to adapt would switch the mode back.
static API::FramebufferInfo apply_display_mode(Optional<DisplayMode> const& display)
{
    if (!display.has_value())
        return {};
    auto* graphics = display->graphics;
    if (display->number != graphics->Mode->Mode && UEFI::is_error(graphics->SetMode(graphics, display->number)))
        return {};

    auto const& mode = *graphics->Mode;
    if (mode.FrameBufferBase == 0)
        return {};
    API::FramebufferInfo framebuffer {};
    framebuffer.physical_base = mode.FrameBufferBase;
    framebuffer.size = mode.FrameBufferSize;
    framebuffer.width = mode.Info->HorizontalResolution;
    framebuffer.height = mode.Info->VerticalResolution;
    framebuffer.pitch = mode.Info->PixelsPerScanLine * 4;
    framebuffer.format = mode.Info->PixelFormat == UEFI::GraphicsPixelFormat::PixelBlueGreenRedReserved8BitPerColor
        ? API::PixelFormat::BGRX8888
        : API::PixelFormat::RGBX8888;
    return framebuffer;
}

static u32 read_be32(void const* address)
{
    auto const* bytes = static_cast<u8 const*>(address);
    return (u32(bytes[0]) << 24) | (u32(bytes[1]) << 16) | (u32(bytes[2]) << 8) | u32(bytes[3]);
}

// Copies the firmware's device tree into memory the kernel keeps, and finds
// the serial port in it.
static void collect_device_tree(UEFI::BootServices& boot_services, API::BootInfo& boot_info)
{
    auto* firmware_tree = find_configuration_table(UEFI::device_tree_table_guid);
    if (!firmware_tree)
        return;

    constexpr u32 max_device_tree_size = 2 * MiB;
    u32 size = read_be32(static_cast<u8 const*>(firmware_tree) + 4);
    if (size > max_device_tree_size)
        fail("reading the device tree"_sv, Error::from_string_literal("implausibly large device tree"));

    auto copy = unwrap("copying the device tree"_sv, allocate_pages(boot_services, size, LoaderMemoryType::LoaderReclaimable));
    __builtin_memcpy(reinterpret_cast<void*>(copy), firmware_tree, size);
    boot_info.device_tree = { copy, size };

    auto tree = unwrap("reading the device tree"_sv, DeviceTree::FlattenedDeviceTree::parse(ReadonlyBytes(reinterpret_cast<u8 const*>(copy), size)));
    auto is_pl011 = [&](DeviceTree::Node const& node) {
        auto compatible = tree.find_property(node, "compatible"_sv);
        return compatible.has_value() && compatible->string_list_contains("arm,pl011"_sv);
    };
    auto serial = tree.find_stdout_node();
    if (!serial.has_value() || !is_pl011(serial.value()))
        serial = tree.find_compatible_node("arm,pl011"_sv);
    if (!serial.has_value())
        return;
    if (auto region = tree.reg(serial.value(), 0); region.has_value())
        boot_info.serial = { region->address, API::SerialType::PL011, 0 };
}

static void collect_entropy(UEFI::BootServices& boot_services, API::BootInfo& boot_info)
{
    void* interface = nullptr;
    if (UEFI::is_error(boot_services.LocateProtocol(&UEFI::rng_protocol_guid, nullptr, &interface)) || !interface)
        return;
    auto* rng = static_cast<UEFI::RngProtocol*>(interface);
    if (!UEFI::is_error(rng->GetRNG(rng, nullptr, sizeof(boot_info.entropy), boot_info.entropy)))
        boot_info.entropy_size = sizeof(boot_info.entropy);
}

// How far up the kernel goes from where it is linked: somewhere in the first 1 GiB above that, on
// a 64 KiB boundary, so one of 16384 places. The boot word "nokaslr" leaves it where it was linked,
// for a debugger reading the symbols.
static u64 choose_kernel_slide(UEFI::BootServices& boot_services, Optional<LoadedFile> const& boot_args)
{
    if (boot_args.has_value() && StringView(boot_args->bytes()).contains("nokaslr"_sv))
        return 0;
    constexpr u64 alignment = 64 * KiB;
    constexpr u64 choices = 16384;
    u64 random;
    asm volatile("mrs %0, cntpct_el0" : "=r"(random));
    void* interface = nullptr;
    if (!UEFI::is_error(boot_services.LocateProtocol(&UEFI::rng_protocol_guid, nullptr, &interface)) && interface) {
        u64 from_firmware = 0;
        auto* rng = static_cast<UEFI::RngProtocol*>(interface);
        if (!UEFI::is_error(rng->GetRNG(rng, nullptr, sizeof(from_firmware), reinterpret_cast<u8*>(&from_firmware))))
            random ^= from_firmware;
    }
    return (random % choices) * alignment;
}

// boot-args is a text file; its contents become the kernel command line.
static void set_command_line(API::BootInfo& boot_info, Optional<LoadedFile> const& boot_args)
{
    if (!boot_args.has_value())
        return;
    auto text = StringView(boot_args->bytes()).trim_whitespace();
    size_t length = min(text.length(), sizeof(boot_info.command_line) - 1);
    for (size_t i = 0; i < length; ++i) {
        char character = text[i];
        boot_info.command_line[i] = is_ascii_printable(static_cast<u8>(character)) ? character : ' ';
    }
    boot_info.command_line[length] = '\0';
}

// Maps all RAM the kernel may touch, and firmware MMIO, at direct_map_base.
// "Reserved" memory is left out: on real machines it can be memory that
// faults when touched, even by speculation.
static ErrorOr<void> map_physical_memory(PageTableBuilder& tables, FirmwareMemoryMap const& memory_map)
{
    for (size_t i = 0; i < memory_map.descriptor_count(); ++i) {
        auto const& descriptor = memory_map.descriptor(i);
        auto type = region_type(descriptor.Type);
        if (type == API::MemoryRegionType::Reserved)
            continue;
        auto attribute = type == API::MemoryRegionType::Mmio ? MemoryAttribute::Device : MemoryAttribute::Normal;
        u64 size = descriptor.NumberOfPages * UEFI::page_size;
        TRY(tables.map(API::direct_map_base + descriptor.PhysicalStart, descriptor.PhysicalStart, size, attribute, Access::ReadWrite));
    }
    return {};
}

// The firmware memory type at `address`, if the memory map describes it.
static Optional<API::MemoryRegionType> region_type_at(FirmwareMemoryMap const& memory_map, u64 address)
{
    for (size_t i = 0; i < memory_map.descriptor_count(); ++i) {
        auto const& descriptor = memory_map.descriptor(i);
        if (address >= descriptor.PhysicalStart && address < descriptor.PhysicalStart + descriptor.NumberOfPages * UEFI::page_size)
            return region_type(descriptor.Type);
    }
    return {};
}

static bool is_in_direct_map(FirmwareMemoryMap const& memory_map, u64 address)
{
    auto type = region_type_at(memory_map, address);
    return type.has_value() && type.value() != API::MemoryRegionType::Reserved;
}

// A framebuffer in RAM (as with QEMU's ramfb) is mapped cacheable: whoever
// scans it out reads RAM through the cache too, and a non-cacheable alias
// could leave them reading stale lines. A device framebuffer (a PCI BAR)
// gets the uncached attribute.
static MemoryAttribute framebuffer_attribute(FirmwareMemoryMap const& memory_map, u64 address)
{
    auto type = region_type_at(memory_map, address);
    bool in_ram = type.has_value() && type.value() != API::MemoryRegionType::Mmio;
    return in_ram ? MemoryAttribute::Normal : MemoryAttribute::NormalUncached;
}

static ErrorOr<void> map_kernel(PageTableBuilder& tables, ELF::Image const& image, LoadedKernel const& kernel)
{
    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_LOAD || segment.p_memsz == 0)
            continue;
        auto access = Access::ReadOnly;
        if (segment.p_flags & ELF::PF_X)
            access = Access::ReadExecute;
        else if (segment.p_flags & ELF::PF_W)
            access = Access::ReadWrite;
        u64 const address = segment.p_vaddr + kernel.slide;
        u64 physical = kernel.physical_base + (address - kernel.virtual_base);
        u64 size = align_up<u64>(segment.p_memsz, UEFI::page_size);
        TRY(tables.map(address, physical, size, MemoryAttribute::Normal, access));
    }
    return {};
}

// After the first call, only GetMemoryMap() may be used, so retry by
// refreshing the map with the buffer reserved earlier.
static void exit_boot_services(UEFI::Handle image_handle, UEFI::BootServices& boot_services, FirmwareMemoryMap& memory_map)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (memory_map.refresh(boot_services).is_error())
            break;
        if (!UEFI::is_error(boot_services.ExitBootServices(image_handle, memory_map.map_key())))
            return;
    }
    // The console may already be gone; all that is left is to stop.
    for (;;)
        asm volatile("wfi");
}

[[noreturn]] static void boot(UEFI::Handle image_handle, Console& console)
{
    auto& boot_services = *s_system_table->BootServices;
    unwrap("checking the CPU"_sv, AArch64::check_cpu());

    auto volume = unwrap("opening the boot volume"_sv, BootVolume::open(image_handle, boot_services));
    auto kernel_file = unwrap("reading the kernel"_sv, volume.load(kernel_path, LoaderMemoryType::KernelFile));
    if (!kernel_file.has_value())
        fail("reading the kernel"_sv, Error::from_string_literal("\\System\\Library\\Kernels\\vulture is missing"));
    auto image = unwrap("reading the kernel"_sv, ELF::Image::parse(kernel_file->bytes()));
    auto boot_args = unwrap("reading boot-args"_sv, volume.load(boot_args_path, LoaderMemoryType::LoaderReclaimable));
    auto kernel = unwrap("loading the kernel"_sv, load_kernel(image, boot_services, choose_kernel_slide(boot_services, boot_args)));
    auto boot_archive = unwrap("reading the boot archive"_sv, volume.load(boot_archive_path, LoaderMemoryType::BootArchive));

    // BootInfo and the memory map array share one allocation; the array is
    // sized for the largest map the reserved buffer can hold.
    auto memory_map = unwrap("reading the memory map"_sv, FirmwareMemoryMap::create(boot_services));
    size_t regions_offset = align_up<size_t>(sizeof(API::BootInfo), 16);
    size_t region_capacity = memory_map.descriptor_capacity();
    auto boot_info_address = unwrap("allocating boot information"_sv,
        allocate_pages(boot_services, regions_offset + region_capacity * sizeof(API::MemoryRegion), LoaderMemoryType::LoaderReclaimable));
    auto& boot_info = *reinterpret_cast<API::BootInfo*>(boot_info_address);
    auto* regions = reinterpret_cast<API::MemoryRegion*>(boot_info_address + regions_offset);

    boot_info.magic = API::boot_info_magic;
    boot_info.version = API::boot_info_version;
    boot_info.size = sizeof(API::BootInfo);
    boot_info.memory_map = boot_info_address + regions_offset;
    boot_info.kernel_image = { kernel.physical_base, kernel.size };
    boot_info.kernel_virtual_base = kernel.virtual_base;
    boot_info.kernel_slide = kernel.slide;
    boot_info.kernel_file = { kernel_file->base, kernel_file->size };
    if (boot_archive.has_value())
        boot_info.boot_archive = { boot_archive->base, boot_archive->size };
    if (auto* rsdp = find_configuration_table(UEFI::acpi_20_table_guid))
        boot_info.acpi_rsdp = reinterpret_cast<u64>(rsdp);
    boot_info.uefi_system_table = reinterpret_cast<u64>(s_system_table);
    collect_device_tree(boot_services, boot_info);
    collect_entropy(boot_services, boot_info);
    set_command_line(boot_info, boot_args);
    auto display = choose_display_mode(boot_services);

    print_label(console, "Kernel"_sv);
    console.println("{} KiB at {:#x}, entry {:#x}", kernel.size / KiB, kernel.physical_base, kernel.entry);
    print_label(console, "Display"_sv);
    if (display.has_value())
        console.println("{}x{}", display->width, display->height);
    else
        console.println("no linear framebuffer");
    print_label(console, "Serial"_sv);
    if (boot_info.serial.type == API::SerialType::PL011)
        console.println("PL011 at {:#x}", boot_info.serial.physical_base);
    else
        console.println("none found");
    print_label(console, "Firmware"_sv);
    console.println("device tree {}, ACPI {}, entropy {}", boot_info.device_tree.size ? "yes"_sv : "no"_sv,
        boot_info.acpi_rsdp ? "yes"_sv : "no"_sv, boot_info.entropy_size ? "yes"_sv : "no"_sv);
    print_label(console, "Boot args"_sv);
    console.println("{}", boot_info.command_line[0] ? StringView(boot_info.command_line) : "(none)"_sv);
    console.println("\nStarting vultureKernel...");

    boot_info.framebuffer = apply_display_mode(display);
    auto const& framebuffer = boot_info.framebuffer;

    // Translation tables. Taking the map snapshot after the allocations above
    // means everything allocated so far is covered by the direct map.
    unwrap("reading the memory map"_sv, memory_map.refresh(boot_services));
    auto kernel_tables = unwrap("creating page tables"_sv, PageTableBuilder::create(boot_services));
    auto identity_tables = unwrap("creating page tables"_sv, PageTableBuilder::create(boot_services));
    unwrap("mapping physical memory"_sv, map_physical_memory(kernel_tables, memory_map));
    unwrap("mapping the kernel"_sv, map_kernel(kernel_tables, image, kernel));

    if (framebuffer.format != API::PixelFormat::None && !is_in_direct_map(memory_map, framebuffer.physical_base)) {
        u64 base = align_down<u64>(framebuffer.physical_base, UEFI::page_size);
        u64 size = align_up<u64>(framebuffer.physical_base + framebuffer.size, UEFI::page_size) - base;
        auto attribute = framebuffer_attribute(memory_map, framebuffer.physical_base);
        unwrap("mapping the framebuffer"_sv, kernel_tables.map(API::direct_map_base + base, base, size, attribute, Access::ReadWrite));
    }
    if (boot_info.serial.type != API::SerialType::None && !is_in_direct_map(memory_map, boot_info.serial.physical_base)) {
        u64 base = align_down<u64>(boot_info.serial.physical_base, UEFI::page_size);
        unwrap("mapping the serial port"_sv, kernel_tables.map(API::direct_map_base + base, base, UEFI::page_size, MemoryAttribute::Device, Access::ReadWrite));
    }

    // The trampoline keeps running at its physical address while the MMU
    // switches over, so it needs an identity mapping (it may cross a page).
    u64 trampoline_page = align_down<u64>(AArch64::trampoline_address(), UEFI::page_size);
    unwrap("mapping the handoff code"_sv, identity_tables.map(trampoline_page, trampoline_page, 2 * UEFI::page_size, MemoryAttribute::Normal, Access::ReadExecute));

    // From here on the firmware is gone: no more console output.
    g_console = nullptr;
    exit_boot_services(image_handle, boot_services, memory_map);
    boot_info.memory_map_entries = convert_memory_map(memory_map, Span<API::MemoryRegion>(regions, region_capacity));

    // Make sure everything the kernel reads first is in memory, and that the
    // copied kernel code is visible to instruction fetch.
    AArch64::synchronize_instruction_cache(kernel.physical_base, kernel.size);
    AArch64::clean_data_cache(kernel.physical_base, kernel.size);
    AArch64::clean_data_cache(boot_info_address, regions_offset + region_capacity * sizeof(API::MemoryRegion));
    AArch64::clean_data_cache(boot_info.device_tree.base, boot_info.device_tree.size);
    for (auto page : kernel_tables.table_pages())
        AArch64::clean_data_cache(page, UEFI::page_size);
    for (auto page : identity_tables.table_pages())
        AArch64::clean_data_cache(page, UEFI::page_size);

    auto state = AArch64::make_handoff_state(identity_tables.root(), kernel_tables.root(), kernel.entry, API::direct_map_base + boot_info_address);
    AArch64::enter_kernel(state);
}

}

extern "C" UEFI::Status efi_main(UEFI::Handle image_handle, UEFI::SystemTable* system_table)
{
    using namespace Boot;

    s_system_table = system_table;
    // The firmware resets the machine after five minutes unless the watchdog is disarmed.
    system_table->BootServices->SetWatchdogTimer(0, 0, 0, nullptr);

    Console console(system_table->ConOut);
    g_console = &console;
    console.clear();
    console.set_color(UEFI::TextColor::White);
    console.println("vultureOS {} - VultureBoot (aarch64)", VULTURE_VERSION_STRING);
    console.set_color(UEFI::TextColor::DarkGray);
    console.println("--------------------------------------------------\n");
    console.set_color(UEFI::TextColor::LightGray);

    boot(image_handle, console);
}
