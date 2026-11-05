/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/input.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Input/Input.h>
#include <Kernel/Devices/VirtIO/VirtIO.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Thread.h>
#include <LibBase/New.h>

// virtio-input: the device writes events into buffers we keep posted. An
// interrupt wakes a thread, which passes them on and posts the buffers again.

namespace Kernel {

void initialize_virtio_input();

namespace {

constexpr u16 device_id = 0x1052;
constexpr u32 config_select_name = 0x01;
constexpr u32 config_offset_select = 0;
constexpr u32 config_offset_subselect = 1;
constexpr u32 config_offset_size = 2;
constexpr u32 config_offset_data = 8;

struct WireEvent {
    u16 type;
    u16 code;
    u32 value;
};
static_assert(sizeof(WireEvent) == 8);

constexpr u16 event_slots = 64;

class VirtIOInput final {
public:
    static ErrorOr<void> create(PCI::Device const& pci)
    {
        auto transport = TRY(try_make<VirtIO::Device>(pci));
        TRY(transport->initialize(0));
        auto queue = TRY(transport->setup_queue(0, event_slots));
        auto memory = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto* device = new (Base::nothrow) VirtIOInput(move(transport), move(queue), memory);
        if (!device)
            return Error::from_errno(ENOMEM);
        auto interrupt = pci.interrupt();
        if (!interrupt.has_value())
            return Error::from_errno(ENODEV);
        register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<VirtIOInput*>(context)->handle_interrupt(); }, device);
        Arch::enable_interrupt(interrupt.value());
        device->m_transport->finish_initialization();
        device->log_name();
        for (u16 slot = 0; slot < event_slots; ++slot)
            TRY(device->post_buffer(slot));
        device->m_transport->notify(*device->m_queue);
        auto thread = TRY(Thread::create("virtio-input"_sv, [](void* context) { static_cast<VirtIOInput*>(context)->run(); }, device, { .qos = QoS::UserInteractive }));
        thread->start();
        return {};
    }

private:
    VirtIOInput(NonnullOwnPtr<VirtIO::Device> transport, NonnullOwnPtr<VirtIO::Queue> queue, PhysicalAddress memory)
        : m_transport(move(transport))
        , m_queue(move(queue))
        , m_memory(memory)
    {
    }

    void log_name()
    {
        m_transport->config_write8(config_offset_select, config_select_name);
        m_transport->config_write8(config_offset_subselect, 0);
        u8 size = min<u8>(m_transport->config_read8(config_offset_size), 63);
        char name[64] = {};
        for (u8 i = 0; i < size; ++i)
            name[i] = static_cast<char>(m_transport->config_read8(config_offset_data + i));
        dmesgln("Input: {}", StringView(name));
        StringView const named { name };
        if (named.ends_with("Tablet"_sv)) {
            m_is_tablet = true;
            m_head = s_tablets++;
        }
    }

    ErrorOr<void> post_buffer(u16 slot)
    {
        VirtIO::Buffer const buffer { m_memory.offset(slot * sizeof(WireEvent)), sizeof(WireEvent), true };
        u16 head = TRY(m_queue->submit({ &buffer, 1 }));
        m_slot_of_head[head] = slot;
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
        auto const* events = reinterpret_cast<WireEvent const*>(physical_to_virtual(m_memory));
        for (;;) {
            m_wait.wait_until([&] { return m_pending.load(); });
            m_pending.store(false);
            bool any = false;
            for (auto completion = m_queue->take_completion(); completion.has_value(); completion = m_queue->take_completion()) {
                u16 slot = m_slot_of_head[completion->id % event_slots];
                auto const& event = events[slot];
                // A tablet is bound to a display, in the order they came.
                if (event.type == VULTURE_EV_ABS && m_is_tablet)
                    post_input_event(VULTURE_EV_HEAD, 0, m_head);
                post_input_event(event.type, event.code, static_cast<i32>(event.value));
                (void)post_buffer(slot);
                any = true;
            }
            if (any)
                m_transport->notify(*m_queue);
        }
    }

    NonnullOwnPtr<VirtIO::Device> m_transport;
    NonnullOwnPtr<VirtIO::Queue> m_queue;
    PhysicalAddress m_memory;
    u16 m_slot_of_head[event_slots] {};
    bool m_is_tablet { false };
    i32 m_head { 0 };
    static inline i32 s_tablets { 0 };
    Atomic<bool> m_pending { false };
    WaitQueue m_wait;
};

}

void initialize_virtio_input()
{
    for (auto& pci : PCI::devices()) {
        if (pci.vendor_id() != VirtIO::vendor_id || pci.device_id() != device_id)
            continue;
        if (auto created = VirtIOInput::create(pci); created.is_error())
            dmesgln("Input: virtio-input at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), created.error());
    }
}

}
