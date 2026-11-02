/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/VirtIO/VirtIO.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Net/NetworkStack.h>
#include <Kernel/Tasks/Thread.h>
#include <LibBase/New.h>

// virtio-net (virtio 1.2, section 5.1): a queue of buffers for frames to arrive in and a queue
// of frames to send, each frame after a header of offloads we don't ask for.

namespace Kernel::Net {

namespace {

constexpr u16 device_id_modern = 0x1041;
constexpr u16 device_id_transitional = 0x1000;

constexpr u16 receive_queue = 0;
constexpr u16 transmit_queue = 1;
constexpr u64 feature_mac = 1ull << 5;

constexpr u32 slot_size = 2048;
constexpr u16 receive_slots = 128;
constexpr u16 transmit_slots = 64;
constexpr unsigned receive_order = 6; // 256 KiB.
constexpr unsigned transmit_order = 5; // 128 KiB.
static_assert(receive_slots * slot_size <= page_size << receive_order);
static_assert(transmit_slots * slot_size <= page_size << transmit_order);

struct [[gnu::packed]] WireHeader {
    u8 flags;
    u8 gso_type;
    u16 header_length;
    u16 gso_size;
    u16 checksum_start;
    u16 checksum_offset;
    u16 buffers;
};
static_assert(sizeof(WireHeader) == 12);

class VirtIONet final : public NetworkInterface {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<VirtIONet>> create(PCI::Device const& pci, String name)
    {
        auto transport = TRY(try_make<VirtIO::Device>(pci));
        u64 features = TRY(transport->initialize(feature_mac));
        auto receive = TRY(transport->setup_queue(receive_queue, receive_slots));
        auto transmit = TRY(transport->setup_queue(transmit_queue, transmit_slots));
        auto receive_memory = TRY(physical_memory().allocate_pages(receive_order, PageUse::General));
        auto transmit_memory = TRY(physical_memory().allocate_pages(transmit_order, PageUse::General));

        MACAddress mac;
        if (features & feature_mac) {
            for (u32 i = 0; i < 6; ++i)
                mac.bytes[i] = transport->config_read8(i);
        } else {
            // A locally administered address, as there is none to read.
            get_random_address(mac);
        }
        auto* raw = new (Base::nothrow) VirtIONet(move(name), mac, move(transport), move(receive), move(transmit), receive_memory, transmit_memory);
        if (!raw)
            return Error::from_errno(ENOMEM);
        auto device = adopt_own(*raw);

        auto interrupt = pci.interrupt();
        if (!interrupt.has_value())
            return Error::from_errno(ENODEV);
        register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<VirtIONet*>(context)->handle_interrupt(); }, raw);
        Arch::enable_interrupt(interrupt.value());

        for (u16 slot = 0; slot < min(receive_slots, raw->m_receive->size()); ++slot)
            TRY(raw->post_receive(slot));
        raw->m_transport->finish_initialization();
        raw->m_transport->notify(*raw->m_receive);

        auto thread = TRY(Thread::create("virtio-net"_sv, [](void* context) { static_cast<VirtIONet*>(context)->run(); }, raw, { .qos = QoS::UserInteractive }));
        thread->start();
        return device;
    }

    ErrorOr<void> transmit(ReadonlyBytes frame) override
    {
        if (frame.size() + sizeof(WireHeader) > slot_size)
            return Error::from_errno(EMSGSIZE);
        // Only ever the one thread here at a time (the stack's lock is held), but the
        // driver's thread gives slots back meanwhile.
        m_room.wait_until([&] { return m_free_transmit_slots.load() != 0; });
        u64 free = m_free_transmit_slots.load();
        u32 slot = static_cast<u32>(__builtin_ctzll(free));
        m_free_transmit_slots.fetch_and(~(1ull << slot));
        auto address = m_transmit_memory.offset(slot * slot_size);
        auto* bytes = physical_to_virtual(address);
        __builtin_memset(bytes, 0, sizeof(WireHeader));
        __builtin_memcpy(bytes + sizeof(WireHeader), frame.data(), frame.size());
        VirtIO::Buffer const buffer { address, static_cast<u32>(sizeof(WireHeader) + frame.size()), false };
        auto head = m_transmit->submit({ &buffer, 1 });
        if (head.is_error()) {
            m_free_transmit_slots.fetch_or(1ull << slot);
            return head.release_error();
        }
        m_slot_of_transmit_head[head.value() % transmit_slots] = static_cast<u8>(slot);
        m_transport->notify(*m_transmit);
        return {};
    }

private:
    VirtIONet(String name, MACAddress mac, NonnullOwnPtr<VirtIO::Device> transport, NonnullOwnPtr<VirtIO::Queue> receive, NonnullOwnPtr<VirtIO::Queue> transmit, PhysicalAddress receive_memory, PhysicalAddress transmit_memory)
        : NetworkInterface(move(name), mac, 1500, false)
        , m_transport(move(transport))
        , m_receive(move(receive))
        , m_transmit(move(transmit))
        , m_receive_memory(receive_memory)
        , m_transmit_memory(transmit_memory)
    {
        u16 slots = min(transmit_slots, m_transmit->size());
        m_free_transmit_slots.store(slots >= 64 ? ~0ull : (1ull << slots) - 1);
    }

    static void get_random_address(MACAddress& mac)
    {
        u64 number = Time::now();
        for (auto& byte : mac.bytes) {
            byte = static_cast<u8>(number);
            number = number * 6364136223846793005ull + 1442695040888963407ull;
        }
        mac.bytes[0] = static_cast<u8>((mac.bytes[0] & 0xfe) | 0x02);
    }

    ErrorOr<void> post_receive(u16 slot)
    {
        VirtIO::Buffer const buffer { m_receive_memory.offset(slot * slot_size), slot_size, true };
        u16 head = TRY(m_receive->submit({ &buffer, 1 }));
        m_slot_of_receive_head[head % receive_slots] = slot;
        return {};
    }

    void handle_interrupt()
    {
        if (!(m_transport->read_interrupt_status() & 1))
            return;
        m_pending.store(true);
        m_wait.wake_all();
    }

    [[noreturn]] void run()
    {
        auto& stack = NetworkStack::the();
        for (;;) {
            m_wait.wait_until([&] { return m_pending.load(); });
            m_pending.store(false);

            bool room = false;
            for (auto done = m_transmit->take_completion(); done.has_value(); done = m_transmit->take_completion()) {
                m_free_transmit_slots.fetch_or(1ull << m_slot_of_transmit_head[done->id % transmit_slots]);
                room = true;
            }
            if (room)
                m_room.wake_all();

            bool received = false;
            for (auto done = m_receive->take_completion(); done.has_value(); done = m_receive->take_completion()) {
                u16 slot = m_slot_of_receive_head[done->id % receive_slots];
                auto* bytes = physical_to_virtual(m_receive_memory.offset(slot * slot_size));
                if (done->length > sizeof(WireHeader))
                    stack.frame_received(*this, { bytes + sizeof(WireHeader), done->length - sizeof(WireHeader) });
                (void)post_receive(slot);
                received = true;
            }
            if (received)
                m_transport->notify(*m_receive);
        }
    }

    NonnullOwnPtr<VirtIO::Device> m_transport;
    NonnullOwnPtr<VirtIO::Queue> m_receive;
    NonnullOwnPtr<VirtIO::Queue> m_transmit;
    PhysicalAddress m_receive_memory;
    PhysicalAddress m_transmit_memory;
    Atomic<u64> m_free_transmit_slots { 0 };
    u16 m_slot_of_receive_head[receive_slots] {};
    u8 m_slot_of_transmit_head[transmit_slots] {};
    Atomic<bool> m_pending { false };
    WaitQueue m_wait;
    WaitQueue m_room;
};

}

void initialize_network_devices()
{
    u32 number = 0;
    for (auto& pci : PCI::devices()) {
        if (pci.vendor_id() != VirtIO::vendor_id || (pci.device_id() != device_id_modern && pci.device_id() != device_id_transitional))
            continue;
        StringBuilder name;
        name.appendff("en{}", number);
        auto created = VirtIONet::create(pci, MUST(name.to_string()));
        if (created.is_error()) {
            dmesgln("Network: virtio-net at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), created.error());
            continue;
        }
        NetworkStack::the().add_interface(adopt_own(*static_cast<NetworkInterface*>(created.release_value().leak_ptr())));
        ++number;
    }
}

}
