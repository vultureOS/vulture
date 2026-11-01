/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/aarch64/MMIO.h>
#include <Kernel/Devices/VirtIO/VirtIO.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <LibBase/New.h>

namespace Kernel::VirtIO {

using namespace AArch64;

namespace {

// Vendor capability types.
constexpr u8 capability_common = 1;
constexpr u8 capability_notify = 2;
constexpr u8 capability_isr = 3;
constexpr u8 capability_device = 4;

// The common configuration structure.
constexpr u32 device_feature_select = 0x00;
constexpr u32 device_feature = 0x04;
constexpr u32 driver_feature_select = 0x08;
constexpr u32 driver_feature = 0x0c;
constexpr u32 msix_config = 0x10;
constexpr u32 device_status = 0x14;
constexpr u32 queue_select = 0x16;
constexpr u32 queue_size = 0x18;
constexpr u32 queue_msix_vector = 0x1a;
constexpr u32 queue_enable = 0x1c;
constexpr u32 queue_notify_off = 0x1e;
constexpr u32 queue_descriptors = 0x20;
constexpr u32 queue_driver = 0x28;
constexpr u32 queue_device = 0x30;

constexpr u8 status_acknowledge = 1;
constexpr u8 status_driver = 2;
constexpr u8 status_driver_ok = 4;
constexpr u8 status_features_ok = 8;

constexpr u16 no_vector = 0xffff;

constexpr u16 descriptor_next = 1;
constexpr u16 descriptor_write = 2;

void write64(FlatPtr address, u64 value)
{
    mmio_write32(address, static_cast<u32>(value));
    mmio_write32(address + 4, static_cast<u32>(value >> 32));
}

}

struct Queue::Descriptor {
    u64 address;
    u32 length;
    u16 flags;
    u16 next;
};

struct AvailableRing {
    u16 flags;
    u16 index;
    u16 ring[];
};

struct UsedElement {
    u32 id;
    u32 length;
};

struct UsedRing {
    u16 flags;
    u16 index;
    UsedElement ring[];
};

// A descriptor is 16 bytes: address, length, flags, next.
static size_t available_offset(u16 size)
{
    return size * 16;
}

static size_t used_offset(u16 size)
{
    return align_up<size_t>(available_offset(size) + 6 + 2 * size, 4);
}

Queue::Queue(u16 index, u16 size, PhysicalAddress memory)
    : m_index(index)
    , m_size(size)
    , m_memory(memory)
{
    // All descriptors start on the free list.
    for (u16 i = 0; i < size; ++i)
        descriptor(i)->next = static_cast<u16>(i + 1);
    m_free_count = size;
}

ErrorOr<NonnullOwnPtr<Queue>> Queue::create(u16 index, u16 size)
{
    size_t bytes = used_offset(size) + 6 + sizeof(UsedElement) * size;
    unsigned order = 0;
    while ((page_size << order) < bytes)
        ++order;
    auto memory = TRY(physical_memory().allocate_pages(order, PageUse::General));
    __builtin_memset(physical_to_virtual(memory), 0, page_size << order);
    auto* queue = new (Base::nothrow) Queue(index, size, memory);
    if (!queue) {
        physical_memory().free_pages(memory);
        return Error::from_errno(ENOMEM);
    }
    return adopt_own(*queue);
}

Queue::~Queue()
{
    physical_memory().free_pages(m_memory);
}

Queue::Descriptor* Queue::descriptor(u16 index) const
{
    static_assert(sizeof(Descriptor) == 16);
    return reinterpret_cast<Descriptor*>(physical_to_virtual(m_memory)) + index;
}

PhysicalAddress Queue::available_ring() const
{
    return m_memory.offset(available_offset(m_size));
}

PhysicalAddress Queue::used_ring() const
{
    return m_memory.offset(used_offset(m_size));
}

ErrorOr<u16> Queue::submit(Span<Buffer const> buffers)
{
    SpinLockGuard guard(m_lock);
    if (buffers.is_empty() || buffers.size() > m_free_count)
        return Error::from_errno(EAGAIN);
    u16 head = m_free_head;
    u16 current = head;
    for (size_t i = 0; i < buffers.size(); ++i) {
        auto* entry = descriptor(current);
        u16 next = entry->next;
        entry->address = buffers[i].address.get();
        entry->length = buffers[i].length;
        entry->flags = static_cast<u16>((buffers[i].device_writes ? descriptor_write : 0) | (i + 1 < buffers.size() ? descriptor_next : 0));
        if (i + 1 < buffers.size())
            entry->next = next;
        current = next;
    }
    m_free_head = current;
    m_free_count = static_cast<u16>(m_free_count - buffers.size());

    auto* available = reinterpret_cast<AvailableRing*>(physical_to_virtual(available_ring()));
    available->ring[available->index % m_size] = head;
    // The descriptors and ring entry before the index the device polls.
    asm volatile("dmb ish" ::: "memory");
    available->index = static_cast<u16>(available->index + 1);
    asm volatile("dsb sy" ::: "memory");
    return head;
}

Optional<Completion> Queue::take_completion()
{
    SpinLockGuard guard(m_lock);
    auto* used = reinterpret_cast<UsedRing volatile*>(physical_to_virtual(used_ring()));
    if (used->index == m_last_used)
        return {};
    // Read the entry only after seeing the index that covers it.
    asm volatile("dmb ish" ::: "memory");
    u32 element_id = used->ring[m_last_used % m_size].id;
    u32 element_length = used->ring[m_last_used % m_size].length;
    m_last_used = static_cast<u16>(m_last_used + 1);
    // Return the chain to the free list.
    u16 id = static_cast<u16>(element_id);
    u16 last = id;
    u16 count = 1;
    while (descriptor(last)->flags & descriptor_next) {
        last = descriptor(last)->next;
        ++count;
    }
    descriptor(last)->next = m_free_head;
    m_free_head = id;
    m_free_count = static_cast<u16>(m_free_count + count);
    return Completion { id, element_length };
}

Device::Device(PCI::Device const& pci)
    : m_pci(pci)
{
}

ErrorOr<u64> Device::initialize(u64 wanted)
{
    // Find and map the configuration structures.
    ErrorOr<void> mapping {};
    m_pci.for_each_capability([&](u8 id, u8 offset) {
        if (id != 0x09 || mapping.is_error())
            return;
        u8 type = m_pci.read8(offset + 3);
        u8 bar_index = m_pci.read8(offset + 4);
        u32 bar_offset = m_pci.read32(offset + 8);
        u32 length = m_pci.read32(offset + 12);
        if (type < capability_common || type > capability_device || bar_index > 5)
            return;
        auto bar = m_pci.bar(bar_index);
        if (!bar.is_memory || bar.address == 0 || static_cast<u64>(bar_offset) + length > bar.size)
            return;
        auto mapped = memory_manager().map_mmio(PhysicalAddress(bar.address + bar_offset), length);
        if (mapped.is_error()) {
            mapping = mapped.release_error();
            return;
        }
        switch (type) {
        case capability_common:
            m_common = mapped.value();
            break;
        case capability_notify:
            m_notify = mapped.value();
            m_notify_multiplier = m_pci.read32(offset + 16);
            break;
        case capability_isr:
            m_isr = mapped.value();
            break;
        case capability_device:
            m_device_config = mapped.value();
            break;
        }
    });
    TRY(mapping);
    if (!m_common || !m_notify || !m_isr)
        return Error::from_string_literal("virtio: the device lacks the modern configuration structures");
    m_pci.enable_memory_and_bus_mastering();

    mmio_write8(m_common + device_status, 0);
    while (mmio_read8(m_common + device_status) != 0)
        Arch::pause();
    mmio_write8(m_common + device_status, status_acknowledge);
    mmio_write8(m_common + device_status, status_acknowledge | status_driver);

    mmio_write32(m_common + device_feature_select, 0);
    u64 offered = mmio_read32(m_common + device_feature);
    mmio_write32(m_common + device_feature_select, 1);
    offered |= static_cast<u64>(mmio_read32(m_common + device_feature)) << 32;
    if (!(offered & feature_version_1))
        return Error::from_string_literal("virtio: the device is legacy-only");
    u64 accepted = offered & (wanted | feature_version_1);
    mmio_write32(m_common + driver_feature_select, 0);
    mmio_write32(m_common + driver_feature, static_cast<u32>(accepted));
    mmio_write32(m_common + driver_feature_select, 1);
    mmio_write32(m_common + driver_feature, static_cast<u32>(accepted >> 32));
    mmio_write8(m_common + device_status, status_acknowledge | status_driver | status_features_ok);
    if (!(mmio_read8(m_common + device_status) & status_features_ok))
        return Error::from_string_literal("virtio: the device rejected the features");
    // Legacy (INTx) interrupts: no MSI-X vectors.
    mmio_write16(m_common + msix_config, no_vector);
    return accepted;
}

ErrorOr<NonnullOwnPtr<Queue>> Device::setup_queue(u16 index, u16 maximum_size)
{
    mmio_write16(m_common + queue_select, index);
    u16 size = mmio_read16(m_common + queue_size);
    if (size == 0)
        return Error::from_string_literal("virtio: the queue does not exist");
    size = min(size, maximum_size);
    auto queue = TRY(Queue::create(index, size));
    mmio_write16(m_common + queue_size, size);
    mmio_write16(m_common + queue_msix_vector, no_vector);
    write64(m_common + queue_descriptors, queue->descriptors().get());
    write64(m_common + queue_driver, queue->available_ring().get());
    write64(m_common + queue_device, queue->used_ring().get());
    queue->notify_offset = mmio_read16(m_common + queue_notify_off);
    mmio_write16(m_common + queue_enable, 1);
    return queue;
}

void Device::finish_initialization()
{
    mmio_write8(m_common + device_status, status_acknowledge | status_driver | status_features_ok | status_driver_ok);
}

void Device::notify(Queue const& queue)
{
    mmio_write16(m_notify + static_cast<FlatPtr>(queue.notify_offset) * m_notify_multiplier, queue.index());
}

u8 Device::read_interrupt_status()
{
    return mmio_read8(m_isr);
}

u8 Device::config_read8(u32 offset) const
{
    return mmio_read8(m_device_config + offset);
}

void Device::config_write8(u32 offset, u8 value)
{
    mmio_write8(m_device_config + offset, value);
}

u32 Device::config_read32(u32 offset) const
{
    return mmio_read32(m_device_config + offset);
}

u64 Device::config_read64(u32 offset) const
{
    // Two halves; re-read if the device changed the value meanwhile.
    u64 low = mmio_read32(m_device_config + offset);
    u64 high = mmio_read32(m_device_config + offset + 4);
    return low | (high << 32);
}

}
