/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Time/Time.h>
#include <Kernel/Arch/MMIO.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/Devices/VirtIO/VirtIO.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <LibBase/New.h>

namespace Kernel {

namespace {

constexpr u64 feature_read_only = 1ull << 5;
constexpr u64 feature_flush = 1ull << 9;

constexpr u32 request_in = 0;
constexpr u32 request_out = 1;
constexpr u32 request_flush = 4;

struct RequestHeader {
    u32 type;
    u32 reserved;
    u64 sector;
};

// One request at a time goes through a DMA page: header, status, and up to
// `transfer_blocks` of data in a bounce buffer.
constexpr size_t transfer_blocks = 128; // 64 KiB
constexpr unsigned bounce_order = 4;

class VirtIOBlockDevice final : public BlockDevice {
public:
    static ErrorOr<NonnullRefPtr<VirtIOBlockDevice>> create(PCI::Device const& pci, String name)
    {
        auto transport = TRY(try_make<VirtIO::Device>(pci));
        u64 features = TRY(transport->initialize(feature_read_only | feature_flush));
        u64 capacity = transport->config_read64(0);
        auto queue = TRY(transport->setup_queue(0, 64));
        auto control = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto bounce = TRY(physical_memory().allocate_pages(bounce_order, PageUse::General));
        auto* device = new (Base::nothrow) VirtIOBlockDevice(move(name), capacity, features, move(transport), move(queue), control, bounce);
        if (!device)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*device);
        if (auto interrupt = pci.interrupt(); interrupt.has_value()) {
            register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<VirtIOBlockDevice*>(context)->handle_interrupt(); }, device);
            Arch::enable_interrupt(interrupt.value());
        }
        reference->m_transport->finish_initialization();
        return reference;
    }

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes buffer) override
    {
        for (size_t done = 0; done < count;) {
            size_t blocks = min(count - done, transfer_blocks);
            TRY(request(request_in, first + done, blocks));
            __builtin_memcpy(buffer.data() + done * block_size, physical_to_virtual(m_bounce), blocks * block_size);
            done += blocks;
        }
        return {};
    }

    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        if (is_read_only())
            return Error::from_errno(EROFS);
        for (size_t done = 0; done < count;) {
            size_t blocks = min(count - done, transfer_blocks);
            __builtin_memcpy(physical_to_virtual(m_bounce), buffer.data() + done * block_size, blocks * block_size);
            TRY(request(request_out, first + done, blocks));
            done += blocks;
        }
        return {};
    }

    ErrorOr<void> flush() override
    {
        if (!(m_features & feature_flush))
            return {};
        return request(request_flush, 0, 0);
    }

private:
    VirtIOBlockDevice(String name, u64 capacity, u64 features, NonnullOwnPtr<VirtIO::Device> transport, NonnullOwnPtr<VirtIO::Queue> queue, PhysicalAddress control, PhysicalAddress bounce)
        : BlockDevice(move(name), capacity, features & feature_read_only)
        , m_features(features)
        , m_transport(move(transport))
        , m_queue(move(queue))
        , m_control(control)
        , m_bounce(bounce)
    {
    }

    ErrorOr<void> request(u32 type, u64 sector, size_t blocks)
    {
        MutexLocker locker(m_lock);
        // In a panic, there may be a request under way: it shares our memory, so it has to end first.
        if (is_panicking()) {
            u64 const deadline = Time::now() + Time::ticks_from_milliseconds(2'000);
            while (!m_done.load() && Time::now() < deadline) {
                if (m_queue->take_completion().has_value())
                    m_done.store(true);
                Arch::pause();
            }
        }
        auto* header = reinterpret_cast<RequestHeader*>(physical_to_virtual(m_control));
        auto* status = physical_to_virtual(m_control) + sizeof(RequestHeader);
        *header = { type, 0, sector };
        *status = 0xff;
        VirtIO::Buffer buffers[3];
        size_t count = 0;
        buffers[count++] = { m_control, sizeof(RequestHeader), false };
        if (blocks > 0)
            buffers[count++] = { m_bounce, static_cast<u32>(blocks * block_size), type == request_in };
        buffers[count++] = { m_control.offset(sizeof(RequestHeader)), 1, true };

        m_done.store(false);
        TRY(m_queue->submit({ buffers, count }));
        m_transport->notify(*m_queue);
        if (is_panicking()) {
            // Nothing will interrupt us: its status says when it has been done.
            u64 const deadline = Time::now() + Time::ticks_from_milliseconds(10'000);
            while (*const_cast<u8 volatile*>(status) == 0xff) {
                if (Time::now() > deadline)
                    return Error::from_errno(ETIMEDOUT);
                Arch::pause();
            }
            Arch::memory_barrier();
            while (m_queue->take_completion().has_value()) { }
            m_done.store(true);
        } else {
            m_completed.wait_until([&] { return m_done.load(); });
        }
        if (*status != 0)
            return Error::from_errno(EIO);
        return {};
    }

    void handle_interrupt()
    {
        if (!(m_transport->read_interrupt_status() & 1))
            return;
        bool any = false;
        while (m_queue->take_completion().has_value())
            any = true;
        if (any) {
            m_done.store(true);
            m_completed.wake_all();
        }
    }

    u64 m_features;
    NonnullOwnPtr<VirtIO::Device> m_transport;
    NonnullOwnPtr<VirtIO::Queue> m_queue;
    PhysicalAddress m_control;
    PhysicalAddress m_bounce;
    Mutex m_lock;
    // Whether the last request has ended: so yes, before the first.
    Atomic<bool> m_done { true };
    WaitQueue m_completed;
};

}

ErrorOr<NonnullRefPtr<BlockDevice>> create_virtio_block_device(PCI::Device const& pci, String name)
{
    auto device = TRY(VirtIOBlockDevice::create(pci, move(name)));
    return NonnullRefPtr<BlockDevice>(*device);
}

}
