/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Console.h>
#include <Boot/UEFI/UEFI.h>

// VultureBoot, milestone M0: reports what the firmware provides (console,
// memory map, framebuffer, configuration tables), the facts the M1 loader
// will hand to the kernel. Then it waits for a key and powers off.

namespace Boot {

static void print_label(Console& console, StringView label)
{
    console.set_color(UEFI::TextColor::DarkGray);
    console.print("{:<10}", label);
    console.set_color(UEFI::TextColor::LightGray);
}

static void print_firmware(Console& console, UEFI::SystemTable const& system_table)
{
    u32 revision = system_table.Hdr.Revision;
    u32 major = revision >> 16;
    u32 minor = revision & 0xffff;

    print_label(console, "Firmware"_sv);
    console.write_ucs2(system_table.FirmwareVendor);
    console.print(" (revision {:#x}), UEFI {}.{}", system_table.FirmwareRevision, major, minor / 10);
    if (minor % 10 != 0)
        console.print(".{}", minor % 10);
    console.write("\n"_sv);
}

static void print_memory(Console& console, UEFI::BootServices& boot_services)
{
    print_label(console, "Memory"_sv);

    UEFI::UIntN map_size = 0;
    UEFI::UIntN map_key = 0;
    UEFI::UIntN descriptor_size = 0;
    u32 descriptor_version = 0;
    auto status = boot_services.GetMemoryMap(&map_size, nullptr, &map_key, &descriptor_size, &descriptor_version);
    if (status != UEFI::StatusCode::BufferTooSmall) {
        console.println("GetMemoryMap failed ({:#x})", status);
        return;
    }

    // Allocating the buffer can itself add descriptors.
    map_size += 4 * descriptor_size;
    void* buffer = nullptr;
    status = boot_services.AllocatePool(UEFI::MemoryType::LoaderData, map_size, &buffer);
    if (UEFI::is_error(status)) {
        console.println("AllocatePool failed ({:#x})", status);
        return;
    }

    status = boot_services.GetMemoryMap(&map_size, static_cast<UEFI::MemoryDescriptor*>(buffer), &map_key, &descriptor_size, &descriptor_version);
    if (UEFI::is_error(status)) {
        console.println("GetMemoryMap failed ({:#x})", status);
        boot_services.FreePool(buffer);
        return;
    }

    size_t descriptor_count = map_size / descriptor_size;
    u64 usable_pages = 0;
    for (size_t i = 0; i < descriptor_count; ++i) {
        auto const& descriptor = *reinterpret_cast<UEFI::MemoryDescriptor const*>(static_cast<u8 const*>(buffer) + i * descriptor_size);
        switch (static_cast<UEFI::MemoryType>(descriptor.Type)) {
        // Everything the firmware hands back once boot services exit.
        case UEFI::MemoryType::ConventionalMemory:
        case UEFI::MemoryType::BootServicesCode:
        case UEFI::MemoryType::BootServicesData:
        case UEFI::MemoryType::LoaderCode:
        case UEFI::MemoryType::LoaderData:
            usable_pages += descriptor.NumberOfPages;
            break;
        default:
            break;
        }
    }
    boot_services.FreePool(buffer);

    console.println("{} MiB usable after boot, {} map entries", usable_pages * UEFI::page_size / MiB, descriptor_count);
}

static StringView pixel_format_name(UEFI::GraphicsPixelFormat format)
{
    switch (format) {
    case UEFI::GraphicsPixelFormat::PixelRedGreenBlueReserved8BitPerColor:
        return "RGBX8"_sv;
    case UEFI::GraphicsPixelFormat::PixelBlueGreenRedReserved8BitPerColor:
        return "BGRX8"_sv;
    case UEFI::GraphicsPixelFormat::PixelBitMask:
        return "bitmask"_sv;
    case UEFI::GraphicsPixelFormat::PixelBltOnly:
        return "blt-only"_sv;
    }
    return "unknown"_sv;
}

static void print_display(Console& console, UEFI::BootServices& boot_services)
{
    print_label(console, "Display"_sv);

    void* interface = nullptr;
    auto status = boot_services.LocateProtocol(&UEFI::graphics_output_protocol_guid, nullptr, &interface);
    if (UEFI::is_error(status) || !interface) {
        console.println("no graphics output ({:#x})", status);
        return;
    }

    auto const& mode = *static_cast<UEFI::GraphicsOutputProtocol*>(interface)->Mode;
    auto const& info = *mode.Info;
    console.println("{}x{} {}, framebuffer at {:#x} ({} KiB), mode {} of {}",
        info.HorizontalResolution, info.VerticalResolution, pixel_format_name(info.PixelFormat),
        mode.FrameBufferBase, mode.FrameBufferSize / KiB, mode.Mode, mode.MaxMode);
}

static void print_configuration_tables(Console& console, UEFI::SystemTable const& system_table)
{
    bool has_device_tree = false;
    bool has_acpi = false;
    bool has_smbios = false;
    for (size_t i = 0; i < system_table.NumberOfTableEntries; ++i) {
        auto const& guid = system_table.ConfigurationTable[i].VendorGuid;
        has_device_tree |= guid == UEFI::device_tree_table_guid;
        has_acpi |= guid == UEFI::acpi_20_table_guid;
        has_smbios |= guid == UEFI::smbios3_table_guid;
    }

    auto yes_no = [](bool present) { return present ? "yes"_sv : "no"_sv; };
    print_label(console, "Tables"_sv);
    console.println("Device Tree: {}, ACPI 2.0: {}, SMBIOS 3: {} ({} total)",
        yes_no(has_device_tree), yes_no(has_acpi), yes_no(has_smbios), system_table.NumberOfTableEntries);
}

static void wait_for_key(UEFI::SystemTable& system_table)
{
    auto* input = system_table.ConIn;
    if (!input)
        return;
    input->Reset(input, false);
    UEFI::UIntN index = 0;
    system_table.BootServices->WaitForEvent(1, &input->WaitForKey, &index);
    UEFI::InputKey key {};
    input->ReadKeyStroke(input, &key);
}

}

extern "C" UEFI::Status efi_main([[maybe_unused]] UEFI::Handle image_handle, UEFI::SystemTable* system_table)
{
    using namespace Boot;

    Console console(system_table->ConOut);
    g_console = &console;

    // The firmware resets the machine after five minutes unless the watchdog is disarmed.
    system_table->BootServices->SetWatchdogTimer(0, 0, 0, nullptr);

    console.clear();
    console.set_color(UEFI::TextColor::White);
    console.println("vultureOS {} - VultureBoot (aarch64)", VULTURE_VERSION_STRING);
    console.set_color(UEFI::TextColor::DarkGray);
    console.println("--------------------------------------------------\n");
    console.set_color(UEFI::TextColor::LightGray);

    print_firmware(console, *system_table);
    print_memory(console, *system_table->BootServices);
    print_display(console, *system_table->BootServices);
    print_configuration_tables(console, *system_table);

    console.write("\n"_sv);
    console.set_color(UEFI::TextColor::LightGreen);
    console.println("VultureBoot: ready");
    console.set_color(UEFI::TextColor::LightGray);
    console.println("The kernel arrives in milestone M1. Press any key to power off.");

    wait_for_key(*system_table);
    system_table->RuntimeServices->ResetSystem(UEFI::ResetType::EfiResetShutdown, UEFI::StatusCode::Success, 0, nullptr);
    return UEFI::StatusCode::Success;
}
