/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Format.h>
#include <LibBase/Optional.h>
#include <LibBase/Vector.h>

namespace DeviceTree {
class FlattenedDeviceTree;
}

// PCI Express through ECAM (configuration space mapped as memory), as
// described by a "pci-host-ecam-generic" device tree node. The firmware has
// already assigned BAR addresses; legacy INTx interrupts are routed through
// the node's interrupt-map.

namespace Kernel::PCI {

struct Bar {
    u64 address { 0 };
    u64 size { 0 };
    bool is_memory { true };
};

class Device {
public:
    Device(FlatPtr configuration, u8 bus, u8 slot, u8 function);

    [[nodiscard]] u8 bus() const { return m_bus; }
    [[nodiscard]] u8 slot() const { return m_slot; }
    [[nodiscard]] u8 function() const { return m_function; }
    [[nodiscard]] u16 vendor_id() const { return read16(0x00); }
    [[nodiscard]] u16 device_id() const { return read16(0x02); }
    [[nodiscard]] u8 class_code() const { return read8(0x0b); }
    [[nodiscard]] u8 subclass() const { return read8(0x0a); }
    [[nodiscard]] u8 interrupt_pin() const { return read8(0x3d); }

    [[nodiscard]] u8 read8(u16 offset) const;
    [[nodiscard]] u16 read16(u16 offset) const;
    [[nodiscard]] u32 read32(u16 offset) const;
    void write16(u16 offset, u16) const;
    void write32(u16 offset, u32) const;

    // Decodes (and sizes) a base address register.
    [[nodiscard]] Bar bar(u8 index) const;
    void enable_memory_and_bus_mastering() const;

    // Vendor-specific capabilities (ID 0x09), by their offset.
    template<typename Callback>
    void for_each_capability(Callback callback) const
    {
        if (!(read16(0x06) & (1 << 4)))
            return;
        u8 offset = read8(0x34) & ~3;
        for (int count = 0; offset && count < 48; ++count) {
            callback(read8(offset), offset);
            offset = read8(offset + 1) & ~3;
        }
    }

    // The GIC interrupt its INTx pin is wired to.
    [[nodiscard]] Optional<u32> interrupt() const { return m_interrupt; }

private:
    friend ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const&);

    FlatPtr m_configuration;
    u8 m_bus;
    u8 m_slot;
    u8 m_function;
    Optional<u32> m_interrupt;
};

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const&);
[[nodiscard]] Vector<Device>& devices();
void describe(FormatSink&);

}
