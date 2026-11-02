/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Bus/PCI/PCI.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/OwnPtr.h>

// virtio 1.x over PCI ("modern" devices): the transport, configured through
// vendor capabilities, and split virtqueues. Devices are cache-coherent on
// this platform; ordering barriers are still needed.

namespace Kernel::VirtIO {

constexpr u16 vendor_id = 0x1af4;
constexpr u64 feature_version_1 = 1ull << 32;

struct Buffer {
    PhysicalAddress address;
    u32 length { 0 };
    bool device_writes { false };
};

struct Completion {
    u16 id { 0 };
    u32 length { 0 };
};

class Queue {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Queue>> create(u16 index, u16 size);
    ~Queue();

    // Puts a chain of buffers in the available ring; returns its head id.
    ErrorOr<u16> submit(Span<Buffer const>);
    // A buffer chain the device has finished with, if any.
    [[nodiscard]] Optional<Completion> take_completion();

    [[nodiscard]] u16 index() const { return m_index; }
    [[nodiscard]] u16 size() const { return m_size; }
    [[nodiscard]] PhysicalAddress descriptors() const { return m_memory; }
    [[nodiscard]] PhysicalAddress available_ring() const;
    [[nodiscard]] PhysicalAddress used_ring() const;

    u16 notify_offset { 0 };

private:
    Queue(u16 index, u16 size, PhysicalAddress memory);

    struct Descriptor;
    Descriptor* descriptor(u16) const;

    SpinLock m_lock;
    u16 m_index;
    u16 m_size;
    PhysicalAddress m_memory;
    u16 m_free_head { 0 };
    u16 m_free_count { 0 };
    u16 m_last_used { 0 };
};

class Device {
public:
    explicit Device(PCI::Device const&);
    virtual ~Device() = default;

    // Resets the device and negotiates `wanted` features (VERSION_1 is
    // always required). Returns the features both sides support.
    ErrorOr<u64> initialize(u64 wanted);
    ErrorOr<NonnullOwnPtr<Queue>> setup_queue(u16 index, u16 maximum_size);
    void finish_initialization();
    void notify(Queue const&);
    // Reading the ISR status acknowledges a legacy interrupt.
    [[nodiscard]] u8 read_interrupt_status();

    [[nodiscard]] u8 config_read8(u32 offset) const;
    void config_write8(u32 offset, u8 value);
    [[nodiscard]] u32 config_read32(u32 offset) const;
    [[nodiscard]] u64 config_read64(u32 offset) const;

    [[nodiscard]] PCI::Device const& pci() const { return m_pci; }

private:
    PCI::Device m_pci;
    FlatPtr m_common { 0 };
    FlatPtr m_notify { 0 };
    FlatPtr m_isr { 0 };
    u16 m_message_vector { 0xffff };
    FlatPtr m_device_config { 0 };
    u32 m_notify_multiplier { 0 };
};

}
