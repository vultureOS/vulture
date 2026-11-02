/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Firmware/ACPI.h>
#include <Kernel/Memory/MemoryManager.h>
#include <LibBase/StringView.h>

namespace Kernel::ACPI {

namespace {

struct [[gnu::packed]] RootPointer {
    char signature[8];
    u8 checksum;
    char oem[6];
    u8 revision;
    u32 rsdt;
    u32 length;
    u64 xsdt;
    u8 extended_checksum;
    u8 reserved[3];
};

struct [[gnu::packed]] TableHeader {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem[6];
    char oem_table[8];
    u32 oem_revision;
    u32 creator;
    u32 creator_revision;
};
static_assert(sizeof(TableHeader) == 36);

struct [[gnu::packed]] GenericAddress {
    u8 space;
    u8 bit_width;
    u8 bit_offset;
    u8 access_size;
    u64 address;
};
static_assert(sizeof(GenericAddress) == 12);

bool s_available = false;
Vector<LocalApic> s_local_apics;
Vector<IoApic> s_io_apics;
Vector<InterruptOverride> s_overrides;
u64 s_local_apic_address = 0xfee0'0000;
FixedDescription s_fixed;
Vector<PciSegment> s_pci_segments;
Optional<u64> s_hpet;
ReadonlyBytes s_dsdt;

// Firmware tables are in memory that's mapped; other than the direct map, they're only ever read
// through this. (Some firmware puts them in what it calls reserved memory, which isn't in that map.)
u8 const* map_table(u64 physical, size_t length)
{
    u64 const start = align_down<u64>(physical, page_size);
    u64 const end = align_up<u64>(physical + length, page_size);
    auto mapped = memory_manager().map_mmio(PhysicalAddress(start), end - start, Arch::MemoryType::Normal);
    if (mapped.is_error())
        return nullptr;
    return reinterpret_cast<u8 const*>(mapped.value() + (physical - start));
}

bool sums_to_zero(u8 const* bytes, size_t length)
{
    u8 sum = 0;
    for (size_t i = 0; i < length; ++i)
        sum = static_cast<u8>(sum + bytes[i]);
    return sum == 0;
}

template<typename T>
T read_at(u8 const* bytes, size_t offset)
{
    T value;
    __builtin_memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

void read_madt(u8 const* table, size_t length)
{
    s_local_apic_address = read_at<u32>(table, 36);
    size_t at = 44;
    while (at + 2 <= length) {
        u8 const type = table[at];
        u8 const size = table[at + 1];
        if (size < 2 || at + size > length)
            break;
        u8 const* entry = table + at;
        switch (type) {
        case 0: // Processor local APIC.
            // Enabled, or could be: but it's only what's enabled that's there to start.
            if (size >= 8 && (read_at<u32>(entry, 4) & 1))
                s_local_apics.append({ entry[3] });
            break;
        case 1: // I/O APIC.
            if (size >= 12)
                s_io_apics.append({ entry[2], read_at<u32>(entry, 4), read_at<u32>(entry, 8) });
            break;
        case 2: { // Interrupt source override.
            if (size < 10)
                break;
            u16 const flags = read_at<u16>(entry, 8);
            s_overrides.append({ entry[3], read_at<u32>(entry, 4), (flags & 3) == 3, ((flags >> 2) & 3) == 3 });
            break;
        }
        case 5: // A wider address for the local APIC.
            if (size >= 12)
                s_local_apic_address = read_at<u64>(entry, 4);
            break;
        case 9: // Processor local x2APIC, for IDs that won't fit the above.
            if (size >= 16 && (read_at<u32>(entry, 8) & 1))
                s_local_apics.append({ read_at<u32>(entry, 4) });
            break;
        default:
            break;
        }
        at += size;
    }
}

void read_fadt(u8 const* table, size_t length)
{
    if (length < 116)
        return;
    s_fixed.smi_command = read_at<u32>(table, 48);
    s_fixed.acpi_enable = table[52];
    s_fixed.pm1a_control = read_at<u32>(table, 64);
    s_fixed.pm1b_control = read_at<u32>(table, 68);
    if (length >= 110) {
        // In the boot architecture flags: bit 1, whether there's a PS/2 controller to be found.
        // Revision 2 onwards; anything older has one by definition.
        if (table[8] >= 2)
            s_fixed.has_i8042 = read_at<u16>(table, 109) & (1 << 1);
    }
    u32 const flags = read_at<u32>(table, 112);
    constexpr u32 reset_register_supported = 1 << 10;
    if ((flags & reset_register_supported) && length >= 129) {
        auto const reset = read_at<GenericAddress>(table, 116);
        if (reset.address != 0 && (reset.space == 0 || reset.space == 1)) {
            s_fixed.reset_register = Register { reset.space, reset.address };
            s_fixed.reset_value = table[128];
        }
    }
    // The bytecode: the wide address if there is one.
    u64 dsdt = read_at<u32>(table, 40);
    if (length >= 148) {
        u64 const wide = read_at<u64>(table, 140);
        if (wide != 0)
            dsdt = wide;
    }
    if (dsdt != 0) {
        auto const* header = map_table(dsdt, sizeof(TableHeader));
        if (header) {
            u32 const total = read_at<u32>(header, 4);
            auto const* whole = map_table(dsdt, total);
            if (whole && total > sizeof(TableHeader))
                s_dsdt = { whole + sizeof(TableHeader), total - sizeof(TableHeader) };
        }
    }
}

void read_mcfg(u8 const* table, size_t length)
{
    for (size_t at = 44; at + 16 <= length; at += 16)
        s_pci_segments.append({ read_at<u64>(table, at), read_at<u16>(table, at + 8), table[at + 10], table[at + 11] });
}

void read_table(u64 physical)
{
    auto const* header = map_table(physical, sizeof(TableHeader));
    if (!header)
        return;
    u32 const length = read_at<u32>(header, 4);
    if (length < sizeof(TableHeader) || length > 16 * MiB)
        return;
    auto const* table = map_table(physical, length);
    if (!table || !sums_to_zero(table, length)) {
        dmesgln("ACPI: the table at {:#x} is damaged", physical);
        return;
    }
    StringView const signature { reinterpret_cast<char const*>(table), 4 };
    if (signature == "APIC"_sv)
        read_madt(table, length);
    else if (signature == "FACP"_sv)
        read_fadt(table, length);
    else if (signature == "MCFG"_sv)
        read_mcfg(table, length);
    else if (signature == "HPET"_sv && length >= 56)
        s_hpet = read_at<GenericAddress>(table, 40).address;
}

}

bool initialize(PhysicalAddress rsdp_address)
{
    if (rsdp_address.get() == 0)
        return false;
    auto const* bytes = map_table(rsdp_address.get(), sizeof(RootPointer));
    if (!bytes)
        return false;
    auto const rsdp = read_at<RootPointer>(bytes, 0);
    if (__builtin_memcmp(rsdp.signature, "RSD PTR ", 8) != 0 || !sums_to_zero(bytes, 20)) {
        dmesgln("ACPI: the root pointer isn't one");
        return false;
    }
    bool const wide = rsdp.revision >= 2 && rsdp.xsdt != 0;
    u64 const root = wide ? rsdp.xsdt : rsdp.rsdt;
    size_t const entry_size = wide ? 8 : 4;
    auto const* root_header = map_table(root, sizeof(TableHeader));
    if (!root_header)
        return false;
    u32 const length = read_at<u32>(root_header, 4);
    auto const* table = map_table(root, length);
    if (!table || !sums_to_zero(table, length)) {
        dmesgln("ACPI: the root table is damaged");
        return false;
    }
    for (size_t at = sizeof(TableHeader); at + entry_size <= length; at += entry_size)
        read_table(wide ? read_at<u64>(table, at) : read_at<u32>(table, at));
    s_available = true;
    dmesgln("ACPI: {} processor{}, {} I/O APIC{}, PCI in {} segment{}, HPET {}, bytecode {} bytes", s_local_apics.size(), s_local_apics.size() == 1 ? ""_sv : "s"_sv,
        s_io_apics.size(), s_io_apics.size() == 1 ? ""_sv : "s"_sv, s_pci_segments.size(), s_pci_segments.size() == 1 ? ""_sv : "s"_sv,
        s_hpet.has_value() ? "yes"_sv : "no"_sv, s_dsdt.size());
    return true;
}

bool is_available()
{
    return s_available;
}

Vector<LocalApic> const& local_apics()
{
    return s_local_apics;
}

Vector<IoApic> const& io_apics()
{
    return s_io_apics;
}

Vector<InterruptOverride> const& interrupt_overrides()
{
    return s_overrides;
}

u64 local_apic_address()
{
    return s_local_apic_address;
}

FixedDescription const& fixed_description()
{
    return s_fixed;
}

Vector<PciSegment> const& pci_segments()
{
    return s_pci_segments;
}

Optional<u64> hpet_address()
{
    return s_hpet;
}

ReadonlyBytes differentiated_description()
{
    return s_dsdt;
}

}
