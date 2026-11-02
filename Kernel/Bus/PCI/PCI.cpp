/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/MMIO.h>
#include <Kernel/Bus/PCI/PCI.h>
#if defined(__x86_64__)
#    include <Kernel/Arch/x86_64/APIC.h>
#    include <Kernel/Firmware/ACPI.h>
#endif
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Memory/MemoryManager.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace Kernel::PCI {

using Arch::mmio_read16;
using Arch::mmio_read32;
using Arch::mmio_read8;
using Arch::mmio_write16;
using Arch::mmio_write32;

static Vector<Device> s_devices;
static u64 s_ecam_physical = 0;
static u32 s_bus_count = 0;

Device::Device(FlatPtr configuration, u8 bus, u8 slot, u8 function)
    : m_configuration(configuration)
    , m_bus(bus)
    , m_slot(slot)
    , m_function(function)
{
}

u8 Device::read8(u16 offset) const { return mmio_read8(m_configuration + offset); }
u16 Device::read16(u16 offset) const { return mmio_read16(m_configuration + offset); }
u32 Device::read32(u16 offset) const { return mmio_read32(m_configuration + offset); }
void Device::write16(u16 offset, u16 value) const { mmio_write16(m_configuration + offset, value); }
void Device::write32(u16 offset, u32 value) const { mmio_write32(m_configuration + offset, value); }

Bar Device::bar(u8 index) const
{
    u16 offset = static_cast<u16>(0x10 + index * 4);
    u32 low = read32(offset);
    Bar bar;
    bar.is_memory = !(low & 1);
    if (!bar.is_memory)
        return bar; // I/O space: not used on this platform.
    bool is_64_bit = ((low >> 1) & 3) == 2;
    u64 address = low & ~0xfull;
    if (is_64_bit)
        address |= static_cast<u64>(read32(offset + 4)) << 32;

    // Sizing: write all ones, read back the mask, restore. Decoding is off
    // meanwhile, so the device never answers at a bogus address.
    u16 command = read16(0x04);
    write16(0x04, command & ~0x3);
    write32(offset, 0xffff'ffff);
    u64 mask = read32(offset) & ~0xfull;
    write32(offset, low);
    if (is_64_bit) {
        u32 high = read32(offset + 4);
        write32(offset + 4, 0xffff'ffff);
        mask |= static_cast<u64>(read32(offset + 4)) << 32;
        write32(offset + 4, high);
    } else {
        mask |= 0xffff'ffff'0000'0000ull;
    }
    write16(0x04, command);
    bar.address = address;
    bar.size = mask ? (~mask + 1) : 0;
    return bar;
}

void Device::enable_memory_and_bus_mastering() const
{
    // Memory space, bus master; legacy interrupts left enabled.
    write16(0x04, (read16(0x04) | 0x2 | 0x4) & ~(1 << 10));
}

// Goes through every function of every device on these buses.
template<typename Callback>
static ErrorOr<void> scan_buses(FlatPtr base, u32 first_bus, u32 last_bus, Callback callback)
{
    for (u32 bus = first_bus; bus <= last_bus; ++bus) {
        for (u8 slot = 0; slot < 32; ++slot) {
            for (u8 function = 0; function < 8; ++function) {
                FlatPtr configuration = base + ((static_cast<FlatPtr>(bus - first_bus) << 20) | (static_cast<FlatPtr>(slot) << 15) | (static_cast<FlatPtr>(function) << 12));
                if (mmio_read16(configuration) == 0xffff) {
                    if (function == 0)
                        break;
                    continue;
                }
                Device device(configuration, static_cast<u8>(bus), slot, function);
                callback(device);
                TRY(s_devices.try_append(device));
                // Single-function devices answer for function 0 only.
                if (function == 0 && !(device.read8(0x0e) & 0x80))
                    break;
            }
        }
    }
    return {};
}

#if defined(__x86_64__)

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const&)
{
    for (auto const& segment : ACPI::pci_segments()) {
        // Only one, on anything this runs on. (Servers can have more, which would take numbering them.)
        if (segment.segment != 0)
            continue;
        s_ecam_physical = segment.base;
        s_bus_count = static_cast<u32>(segment.last_bus) - segment.first_bus + 1;
        // The table's address is for bus 0, whichever the first it covers.
        u64 const physical = segment.base + (static_cast<u64>(segment.first_bus) << 20);
        FlatPtr const base = TRY(memory_manager().map_mmio(PhysicalAddress(physical), static_cast<size_t>(s_bus_count) << 20));
        TRY(scan_buses(base, segment.first_bus, segment.last_bus, [](Device&) { }));
    }
    if (s_bus_count == 0)
        return Error::from_string_literal("no PCI Express segment in the ACPI tables");
    return {};
}

namespace {

// (Drivers keep copies of a Device, and they're all the one device.)
struct MessageInterrupt {
    u32 location;
    u32 vector;
};
Vector<MessageInterrupt> s_message_interrupts;

}

bool Device::uses_message_table() const
{
    bool enabled = false;
    for_each_capability([&](u8 id, u8 offset) {
        if (id == 0x11)
            enabled = read16(offset + 2) & (1 << 15);
    });
    return enabled;
}

Optional<u32> Device::interrupt() const
{
    u32 const location = (static_cast<u32>(m_bus) << 16) | (static_cast<u32>(m_slot) << 8) | m_function;
    for (auto const& known : s_message_interrupts) {
        if (known.location == location)
            return known.vector;
    }
    Optional<u8> plain;
    Optional<u8> table;
    for_each_capability([&](u8 id, u8 offset) {
        if (id == 0x05)
            plain = offset;
        else if (id == 0x11)
            table = offset;
    });
    if (!plain.has_value() && !table.has_value()) {
        dmesgln("PCI: {:02x}:{:02x}.{} has no message interrupts", m_bus, m_slot, m_function);
        return {};
    }
    auto vector = X86_64::allocate_message_vector();
    if (vector.is_error()) {
        dmesgln("PCI: {:02x}:{:02x}.{}: {}", m_bus, m_slot, m_function, vector.error());
        return {};
    }
    u64 const address = X86_64::message_address();
    u32 const data = X86_64::message_data(vector.value());
    if (plain.has_value()) {
        u16 const offset = plain.value();
        u16 const control = read16(offset + 2);
        write32(offset + 4, static_cast<u32>(address));
        bool const wide = control & (1 << 7);
        if (wide) {
            write32(offset + 8, static_cast<u32>(address >> 32));
            write16(offset + 12, static_cast<u16>(data));
        } else {
            write16(offset + 8, static_cast<u16>(data));
        }
        // One vector, on: nothing else in the control word is ours to choose.
        write16(offset + 2, (control & ~static_cast<u16>(0x70)) | 1);
    } else {
        u16 const offset = table.value();
        u16 const control = read16(offset + 2);
        u32 const table_location = read32(offset + 4);
        auto const bar_of_table = this->bar(static_cast<u8>(table_location & 7));
        u64 const physical = bar_of_table.address + (table_location & ~7u);
        // Every entry starts masked; the only one used is the first.
        auto const mapped = memory_manager().map_mmio(PhysicalAddress(align_down<u64>(physical, page_size)), page_size);
        if (mapped.is_error()) {
            dmesgln("PCI: {:02x}:{:02x}.{}: {}", m_bus, m_slot, m_function, mapped.error());
            return {};
        }
        FlatPtr const entry = mapped.value() + (physical % page_size);
        mmio_write32(entry + 0, static_cast<u32>(address));
        mmio_write32(entry + 4, static_cast<u32>(address >> 32));
        mmio_write32(entry + 8, data);
        mmio_write32(entry + 12, 0);
        write16(offset + 2, (control | (1 << 15)) & ~static_cast<u16>(1 << 14));
    }
    // The old way of interrupting, off.
    write16(0x04, read16(0x04) | (1 << 10));
    s_message_interrupts.append({ location, vector.value() });
    return vector.value();
}

#else

// The GIC interrupt for a device's INTx pin, from the host bridge's
// interrupt-map (masked by interrupt-map-mask).
static Optional<u32> route_interrupt(DeviceTree::FlattenedDeviceTree const& tree, DeviceTree::Node const& bridge, u8 bus, u8 slot, u8 function, u8 pin)
{
    auto map = tree.find_property(bridge, "interrupt-map"_sv);
    auto mask = tree.find_property(bridge, "interrupt-map-mask"_sv);
    if (!map.has_value() || !mask.has_value() || pin == 0)
        return {};
    u32 address_cells = tree.u32_property(bridge, "#address-cells"_sv, 3);
    u32 interrupt_cells = tree.u32_property(bridge, "#interrupt-cells"_sv, 1);
    if (address_cells != 3 || interrupt_cells != 1)
        return {};
    u32 key[4] = { (static_cast<u32>(bus) << 16) | (static_cast<u32>(slot) << 11) | (static_cast<u32>(function) << 8), 0, 0, pin };
    u32 key_mask[4];
    for (size_t i = 0; i < 4; ++i)
        key_mask[i] = static_cast<u32>(mask->read_cells(i, 1).value_or(0xffff'ffff));

    size_t cell = 0;
    size_t total = map->cell_count();
    while (cell + 5 <= total) {
        bool matches = true;
        for (size_t i = 0; i < 4; ++i) {
            u32 entry = static_cast<u32>(map->read_cells(cell + i, 1).value_or(0));
            matches = matches && (entry & key_mask[i]) == (key[i] & key_mask[i]);
        }
        u32 phandle = static_cast<u32>(map->read_cells(cell + 4, 1).value_or(0));
        auto parent = tree.find_node_by_phandle(phandle);
        if (!parent.has_value())
            return {};
        u32 parent_address_cells = tree.u32_property(parent.value(), "#address-cells"_sv, 0);
        u32 parent_interrupt_cells = tree.u32_property(parent.value(), "#interrupt-cells"_sv, 3);
        size_t specifier = cell + 5 + parent_address_cells;
        if (matches && parent_interrupt_cells >= 2) {
            // GIC specifier: type (0 = SPI, 1 = PPI), number, flags.
            u32 type = static_cast<u32>(map->read_cells(specifier, 1).value_or(0));
            u32 number = static_cast<u32>(map->read_cells(specifier + 1, 1).value_or(0));
            return (type == 0 ? 32 : 16) + number;
        }
        cell = specifier + parent_interrupt_cells;
    }
    return {};
}

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const& tree)
{
    auto bridge = tree.find_compatible_node("pci-host-ecam-generic"_sv);
    if (!bridge.has_value())
        return Error::from_string_literal("no PCIe host bridge in the device tree");
    auto ecam = tree.reg(bridge.value(), 0);
    if (!ecam.has_value())
        return Error::from_string_literal("the PCIe host bridge has no ECAM region");
    u32 first_bus = 0;
    u32 last_bus = static_cast<u32>(ecam->size >> 20) - 1;
    if (auto range = tree.find_property(bridge.value(), "bus-range"_sv); range.has_value()) {
        first_bus = static_cast<u32>(range->read_cells(0, 1).value_or(0));
        last_bus = static_cast<u32>(range->read_cells(1, 1).value_or(last_bus));
    }
    s_ecam_physical = ecam->address;
    s_bus_count = last_bus - first_bus + 1;
    FlatPtr base = TRY(memory_manager().map_mmio(PhysicalAddress(ecam->address), static_cast<size_t>(s_bus_count) << 20));
    return scan_buses(base, first_bus, last_bus, [&](Device& device) {
        device.m_interrupt = route_interrupt(tree, bridge.value(), device.bus(), device.slot(), device.function(), device.interrupt_pin());
    });
}

Optional<u32> Device::interrupt() const
{
    return m_interrupt;
}

bool Device::uses_message_table() const
{
    return false;
}

#endif

Vector<Device>& devices()
{
    return s_devices;
}

void describe(FormatSink& sink)
{
    format_to(sink, "ECAM at {:#x}, {} bus{}, {} function{}", s_ecam_physical, s_bus_count, s_bus_count == 1 ? ""_sv : "es"_sv,
        s_devices.size(), s_devices.size() == 1 ? ""_sv : "s"_sv);
}

}
