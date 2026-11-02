/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/Types.h>

// USB, as far as the devices on it go. A controller (XHCI.cpp) finds them, reads their descriptors, and
// hands each interface it recognizes to a class driver (HID.cpp, MassStorage.cpp), which talks to it with
// what's here.

namespace Kernel::USB {

constexpr u8 class_hid = 0x03;
constexpr u8 class_mass_storage = 0x08;
constexpr u8 class_hub = 0x09;

constexpr u8 descriptor_device = 1;
constexpr u8 descriptor_configuration = 2;
constexpr u8 descriptor_interface = 4;
constexpr u8 descriptor_endpoint = 5;
constexpr u8 descriptor_hid = 0x21;
constexpr u8 descriptor_report = 0x22;

constexpr u8 request_clear_feature = 1;
constexpr u8 request_get_descriptor = 6;
constexpr u8 request_set_configuration = 9;

enum class EndpointKind : u8 {
    Control = 0,
    Isochronous = 1,
    Bulk = 2,
    Interrupt = 3,
};

struct EndpointDescription {
    u8 address { 0 }; // Bit 7: comes in to here.
    EndpointKind kind { EndpointKind::Control };
    u16 max_packet { 0 };
    u8 interval { 0 };
    [[nodiscard]] bool is_in() const { return address & 0x80; }
};

struct InterfaceDescription {
    u8 number { 0 };
    u8 interface_class { 0 };
    u8 subclass { 0 };
    u8 protocol { 0 };
    u16 report_descriptor_length { 0 };
    u8 endpoint_count { 0 };
    EndpointDescription endpoints[4];
};

class Endpoint;

// One attached device, whatever controller it's on. Everything here waits for the device, apart from what
// says otherwise, and is called from a thread.
class Device {
public:
    virtual ~Device() = default;

    // On the default endpoint. `data` is what it says is either going or coming; returns how much did.
    virtual ErrorOr<size_t> control(u8 request_type, u8 request, u16 value, u16 index, Bytes data) = 0;

    // Sets up the endpoints of an interface that a driver is going to use.
    virtual ErrorOr<Endpoint*> open_endpoint(EndpointDescription const&) = 0;

    // For memory: physically contiguous, within a boundary the hardware can't cross, and known to it.
    // Moves `length` bytes at `buffer`.
    virtual ErrorOr<size_t> bulk(Endpoint&, PhysicalAddress buffer, size_t length) = 0;

    // For what doesn't answer until there is something to say. `callback(context, bytes)` is called on the
    // controller's thread for each report that arrives, for as long as the device lasts.
    using ReportCallback = void (*)(void* context, ReadonlyBytes report);
    virtual ErrorOr<void> poll_interrupts(Endpoint&, size_t report_size, ReportCallback, void* context) = 0;

    // The next steps after an endpoint has stalled.
    virtual ErrorOr<void> clear_stall(Endpoint&) = 0;
};

// The class drivers: false if this isn't an interface it knows how to drive.
ErrorOr<bool> attach_hid(Device&, InterfaceDescription const&);
ErrorOr<bool> attach_mass_storage(Device&, InterfaceDescription const&);

// Finds the controllers on the PCI bus, and what's attached to them by the time this returns.
void initialize();

}
