/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/MMIO.h>
#include <Kernel/Bus/PCI/PCI.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Net/NetworkStack.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Atomic.h>
#include <LibBase/New.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>

// Intel's gigabit Ethernet controllers, of the kind on a great many PCs: two rings of descriptors in
// memory, and tail registers to say how far along they've been filled. Tested against QEMU's 82574L.
// The later ones built into chipsets (I217, I219) use the same registers once running, but may want more
// done to wake their PHY than this does.

namespace Kernel::Net {

namespace {

using Arch::mmio_read32;
using Arch::mmio_write32;

constexpr FlatPtr reg_control = 0x0000;
constexpr FlatPtr reg_interrupt_cause = 0x00c0;
constexpr FlatPtr reg_interrupt_set = 0x00d0;
constexpr FlatPtr reg_interrupt_clear = 0x00d8;
constexpr FlatPtr reg_receive_control = 0x0100;
constexpr FlatPtr reg_transmit_control = 0x0400;
constexpr FlatPtr reg_transmit_gap = 0x0410;
constexpr FlatPtr reg_receive_ring_low = 0x2800;
constexpr FlatPtr reg_receive_ring_high = 0x2804;
constexpr FlatPtr reg_receive_ring_length = 0x2808;
constexpr FlatPtr reg_receive_head = 0x2810;
constexpr FlatPtr reg_receive_tail = 0x2818;
constexpr FlatPtr reg_transmit_ring_low = 0x3800;
constexpr FlatPtr reg_transmit_ring_high = 0x3804;
constexpr FlatPtr reg_transmit_ring_length = 0x3808;
constexpr FlatPtr reg_transmit_head = 0x3810;
constexpr FlatPtr reg_transmit_tail = 0x3818;
constexpr FlatPtr reg_multicast_table = 0x5200;
constexpr FlatPtr reg_address_low = 0x5400;
constexpr FlatPtr reg_address_high = 0x5404;

constexpr u32 control_set_link_up = 1u << 6;
constexpr u32 control_reset = 1u << 26;
constexpr u32 control_link_reset = 1u << 3;

constexpr u32 receive_enable = 1u << 1;
constexpr u32 receive_broadcast = 1u << 15;
constexpr u32 receive_strip_crc = 1u << 26;
constexpr u32 transmit_enable = 1u << 1;
constexpr u32 transmit_pad_short = 1u << 3;
constexpr u32 transmit_collision_threshold = 0x0f << 4;
constexpr u32 transmit_collision_distance = 0x40 << 12;

constexpr u32 cause_transmit_done = 1u << 0;
constexpr u32 cause_link_change = 1u << 2;
constexpr u32 cause_receive_low = 1u << 4;
constexpr u32 cause_receive_timer = 1u << 7;

struct ReceiveDescriptor {
    u64 address;
    u16 length;
    u16 checksum;
    u8 status;
    u8 errors;
    u16 special;
};
static_assert(sizeof(ReceiveDescriptor) == 16);

struct TransmitDescriptor {
    u64 address;
    u16 length;
    u8 checksum_offset;
    u8 command;
    u8 status;
    u8 checksum_start;
    u16 special;
};
static_assert(sizeof(TransmitDescriptor) == 16);

constexpr u8 status_done = 1 << 0;
constexpr u8 status_end_of_packet = 1 << 1;
constexpr u8 command_end_of_packet = 1 << 0;
constexpr u8 command_insert_crc = 1 << 1;
constexpr u8 command_report_status = 1 << 3;

constexpr size_t ring_size = 128;
constexpr size_t slot_size = 2048;
constexpr unsigned rings_order = 1;
constexpr unsigned slots_order = 7; // 512 KiB, for both directions.
static_assert(ring_size * 2 * slot_size <= page_size << slots_order);
static_assert(ring_size * 2 * sizeof(ReceiveDescriptor) <= page_size << rings_order);

bool is_supported(PCI::Device const& pci)
{
    if (pci.vendor_id() != 0x8086 || pci.class_code() != 0x02)
        return false;
    switch (pci.device_id()) {
    case 0x10d3: // 82574L
    case 0x10f6: // 82574LA
    case 0x105e: // 82571EB
    case 0x107d: // 82572EI
    case 0x10bd: // 82566DM
    case 0x10f5: // 82567LM
    case 0x1502: // 82579LM
    case 0x1503: // 82579V
    case 0x153a: // I217-LM
    case 0x153b: // I217-V
    case 0x155a: // I218-LM
    case 0x1559: // I218-V
    case 0x15a0: // I218-LM (2)
    case 0x15a1: // I218-V (2)
    case 0x156f: // I219-LM
    case 0x1570: // I219-V
    case 0x15b7: // I219-LM (2)
    case 0x15b8: // I219-V (2)
        return true;
    default:
        return false;
    }
}

class E1000 final : public NetworkInterface {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<E1000>> create(PCI::Device const& pci, String name)
    {
        auto const bar = pci.bar(0);
        if (!bar.is_memory || bar.address == 0)
            return Error::from_string_literal("no registers");
        pci.enable_memory_and_bus_mastering();
        FlatPtr const registers = TRY(memory_manager().map_mmio(PhysicalAddress(bar.address), bar.size ? bar.size : 128 * KiB));

        mmio_write32(registers + reg_interrupt_clear, 0xffff'ffff);
        mmio_write32(registers + reg_control, mmio_read32(registers + reg_control) | control_reset);
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(100);
        while ((mmio_read32(registers + reg_control) & control_reset) && Time::now() < deadline)
            Arch::pause();
        // (Writes in the first moments after a reset can go missing.)
        Time::wait_until(Time::now() + Time::ticks_from_milliseconds(1));
        mmio_write32(registers + reg_interrupt_clear, 0xffff'ffff);
        (void)mmio_read32(registers + reg_interrupt_cause);

        MACAddress mac;
        u32 const low = mmio_read32(registers + reg_address_low);
        u32 const high = mmio_read32(registers + reg_address_high);
        for (int i = 0; i < 4; ++i)
            mac.bytes[i] = static_cast<u8>(low >> (8 * i));
        mac.bytes[4] = static_cast<u8>(high);
        mac.bytes[5] = static_cast<u8>(high >> 8);

        auto const rings = TRY(physical_memory().allocate_pages(rings_order, PageUse::General));
        auto const slots = TRY(physical_memory().allocate_pages(slots_order, PageUse::General));
        __builtin_memset(physical_to_virtual(rings), 0, page_size << rings_order);

        auto* raw = new (Base::nothrow) E1000(move(name), mac, registers, rings, slots);
        if (!raw)
            return Error::from_errno(ENOMEM);
        auto device = adopt_own(*raw);
        raw->start_rings();

        auto const interrupt = pci.interrupt();
        if (!interrupt.has_value())
            return Error::from_errno(ENODEV);
        register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<E1000*>(context)->handle_interrupt(); }, raw);
        Arch::enable_interrupt(interrupt.value());
        auto thread = TRY(Thread::create("e1000"_sv, [](void* context) { static_cast<E1000*>(context)->run(); }, raw, { .qos = QoS::UserInteractive }));
        thread->start();
        mmio_write32(registers + reg_interrupt_set, cause_receive_timer | cause_receive_low | cause_transmit_done | cause_link_change);
        return device;
    }

    ErrorOr<void> transmit(ReadonlyBytes frame) override
    {
        if (frame.size() > slot_size)
            return Error::from_errno(EMSGSIZE);
        MutexLocker locker(m_transmit_lock);
        auto* ring = transmit_ring();
        auto const tail = m_transmit_tail;
        auto const next = (tail + 1) % ring_size;
        // One gap is left, or a full ring would look like an empty one.
        m_room.wait_until([&] { return (next == m_transmit_reclaimed.load()) == false; });
        __builtin_memcpy(physical_to_virtual(transmit_slot(tail)), frame.data(), frame.size());
        ring[tail].address = transmit_slot(tail).get();
        ring[tail].length = static_cast<u16>(frame.size());
        ring[tail].command = command_end_of_packet | command_insert_crc | command_report_status;
        ring[tail].status = 0;
        m_transmit_tail = next;
        Arch::device_barrier();
        mmio_write32(m_registers + reg_transmit_tail, static_cast<u32>(next));
        return {};
    }

private:
    E1000(String name, MACAddress mac, FlatPtr registers, PhysicalAddress rings, PhysicalAddress slots)
        : NetworkInterface(move(name), mac, 1500, false)
        , m_registers(registers)
        , m_rings(rings)
        , m_slots(slots)
    {
    }

    [[nodiscard]] ReceiveDescriptor* receive_ring() { return reinterpret_cast<ReceiveDescriptor*>(physical_to_virtual(m_rings)); }
    [[nodiscard]] TransmitDescriptor* transmit_ring() { return reinterpret_cast<TransmitDescriptor*>(physical_to_virtual(m_rings.offset(ring_size * sizeof(ReceiveDescriptor)))); }
    [[nodiscard]] PhysicalAddress receive_slot(size_t index) const { return m_slots.offset(index * slot_size); }
    [[nodiscard]] PhysicalAddress transmit_slot(size_t index) const { return m_slots.offset((ring_size + index) * slot_size); }

    void start_rings()
    {
        for (int i = 0; i < 128; ++i)
            mmio_write32(m_registers + reg_multicast_table + i * 4, 0);

        auto* receive = receive_ring();
        for (size_t i = 0; i < ring_size; ++i)
            receive[i].address = receive_slot(i).get();
        u64 const receive_address = m_rings.get();
        mmio_write32(m_registers + reg_receive_ring_low, static_cast<u32>(receive_address));
        mmio_write32(m_registers + reg_receive_ring_high, static_cast<u32>(receive_address >> 32));
        mmio_write32(m_registers + reg_receive_ring_length, ring_size * sizeof(ReceiveDescriptor));
        mmio_write32(m_registers + reg_receive_head, 0);
        mmio_write32(m_registers + reg_receive_tail, ring_size - 1);
        // 2048-byte buffers, which is the setting of zero.
        mmio_write32(m_registers + reg_receive_control, receive_enable | receive_broadcast | receive_strip_crc);

        u64 const transmit_address = m_rings.get() + ring_size * sizeof(ReceiveDescriptor);
        mmio_write32(m_registers + reg_transmit_ring_low, static_cast<u32>(transmit_address));
        mmio_write32(m_registers + reg_transmit_ring_high, static_cast<u32>(transmit_address >> 32));
        mmio_write32(m_registers + reg_transmit_ring_length, ring_size * sizeof(TransmitDescriptor));
        mmio_write32(m_registers + reg_transmit_head, 0);
        mmio_write32(m_registers + reg_transmit_tail, 0);
        mmio_write32(m_registers + reg_transmit_gap, 10 | (10 << 10) | (10 << 20));
        mmio_write32(m_registers + reg_transmit_control, transmit_enable | transmit_pad_short | transmit_collision_threshold | transmit_collision_distance);

        u32 control = mmio_read32(m_registers + reg_control);
        mmio_write32(m_registers + reg_control, (control | control_set_link_up) & ~control_link_reset);
    }

    void handle_interrupt()
    {
        // Reading it clears it, and says whether it's this device's, in case it's shared.
        if (mmio_read32(m_registers + reg_interrupt_cause) == 0)
            return;
        m_pending.store(true);
        m_wait.wake_all();
    }

    [[noreturn]] void run()
    {
        auto& stack = NetworkStack::the();
        auto* receive = receive_ring();
        auto* transmit = transmit_ring();
        for (;;) {
            m_wait.wait_until([&] { return m_pending.load(); });
            m_pending.store(false);

            bool room = false;
            size_t reclaimed = m_transmit_reclaimed.load();
            while (reclaimed != m_transmit_tail && (transmit[reclaimed].status & status_done)) {
                reclaimed = (reclaimed + 1) % ring_size;
                room = true;
            }
            if (room) {
                m_transmit_reclaimed.store(reclaimed);
                m_room.wake_all();
            }

            bool received = false;
            for (;;) {
                auto& descriptor = receive[m_receive_next];
                if (!(descriptor.status & status_done))
                    break;
                Arch::memory_barrier();
                if ((descriptor.status & status_end_of_packet) && descriptor.errors == 0 && descriptor.length > 0)
                    stack.frame_received(*this, { physical_to_virtual(receive_slot(m_receive_next)), descriptor.length });
                descriptor.status = 0;
                descriptor.length = 0;
                m_receive_last_given = m_receive_next;
                m_receive_next = (m_receive_next + 1) % ring_size;
                received = true;
            }
            if (received) {
                Arch::device_barrier();
                mmio_write32(m_registers + reg_receive_tail, static_cast<u32>(m_receive_last_given));
            }
        }
    }

    FlatPtr m_registers;
    PhysicalAddress m_rings;
    PhysicalAddress m_slots;
    Mutex m_transmit_lock;
    size_t m_transmit_tail { 0 };
    Atomic<size_t> m_transmit_reclaimed { 0 };
    size_t m_receive_next { 0 };
    size_t m_receive_last_given { ring_size - 1 };
    Atomic<bool> m_pending { false };
    WaitQueue m_wait;
    WaitQueue m_room;
};

}

void initialize_e1000_devices(u32& number);
void initialize_e1000_devices(u32& number)
{
    for (auto& pci : PCI::devices()) {
        if (!is_supported(pci))
            continue;
        StringBuilder name;
        name.appendff("en{}", number);
        auto created = E1000::create(pci, MUST(name.to_string()));
        if (created.is_error()) {
            dmesgln("Network: e1000 at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), created.error());
            continue;
        }
        NetworkStack::the().add_interface(adopt_own(*static_cast<NetworkInterface*>(created.release_value().leak_ptr())));
        ++number;
    }
}

}
