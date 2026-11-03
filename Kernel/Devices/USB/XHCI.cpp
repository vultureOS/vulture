/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/MMIO.h>
#include <Kernel/Bus/PCI/PCI.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/USB/USB.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Atomic.h>
#include <LibBase/New.h>
#include <LibBase/Vector.h>

// The USB 3 host controller interface: every USB port on a PC of the last decade, and on a Raspberry Pi 4.
// It works from rings of 16-byte "transfer request blocks" in memory: one for commands to it, one of
// events from it, and one for each endpoint of each device. Whoever owns a block is decided by a cycle bit,
// which changes meaning each time a ring goes round.

namespace Kernel::USB {

class Endpoint {
public:
    virtual ~Endpoint() = default;
};

namespace {

using Arch::mmio_read32;
using Arch::mmio_read8;
using Arch::mmio_write32;

constexpr FlatPtr cap_length = 0x00;
constexpr FlatPtr cap_structural_1 = 0x04;
constexpr FlatPtr cap_structural_2 = 0x08;
constexpr FlatPtr cap_capability_1 = 0x10;
constexpr FlatPtr cap_doorbell_offset = 0x14;
constexpr FlatPtr cap_runtime_offset = 0x18;

constexpr FlatPtr op_command = 0x00;
constexpr FlatPtr op_status = 0x04;
constexpr FlatPtr op_command_ring = 0x18;
constexpr FlatPtr op_device_contexts = 0x30;
constexpr FlatPtr op_configure = 0x38;
constexpr FlatPtr op_port_status = 0x400;

constexpr u32 command_run = 1 << 0;
constexpr u32 command_reset = 1 << 1;
constexpr u32 command_interrupts = 1 << 2;
constexpr u32 status_halted = 1 << 0;
constexpr u32 status_not_ready = 1 << 11;

constexpr FlatPtr interrupter_0 = 0x20;
constexpr FlatPtr interrupter_management = 0x00;
constexpr FlatPtr interrupter_moderation = 0x04;
constexpr FlatPtr interrupter_table_size = 0x08;
constexpr FlatPtr interrupter_table = 0x10;
constexpr FlatPtr interrupter_dequeue = 0x18;
constexpr u32 management_pending = 1 << 0;
constexpr u32 management_enable = 1 << 1;
constexpr u64 dequeue_busy = 1 << 3;

constexpr u32 port_connected = 1 << 0;
constexpr u32 port_enabled = 1 << 1;
constexpr u32 port_reset = 1 << 4;
constexpr u32 port_power = 1 << 9;
constexpr u32 port_changes = 0x7f << 17;
constexpr u32 port_reset_changed = 1 << 21;

// Block types.
constexpr u32 trb_normal = 1;
constexpr u32 trb_setup = 2;
constexpr u32 trb_data = 3;
constexpr u32 trb_status = 4;
constexpr u32 trb_link = 6;
constexpr u32 trb_enable_slot = 9;
constexpr u32 trb_disable_slot = 10;
constexpr u32 trb_address_device = 11;
constexpr u32 trb_configure_endpoint = 12;
constexpr u32 trb_evaluate_context = 13;
constexpr u32 trb_reset_endpoint = 14;
constexpr u32 trb_set_dequeue = 16;
constexpr u32 trb_event_transfer = 32;
constexpr u32 trb_event_command = 33;
constexpr u32 trb_event_port = 34;

constexpr u32 control_cycle = 1 << 0;
constexpr u32 control_toggle_cycle = 1 << 1;
constexpr u32 control_interrupt_short = 1 << 2;
constexpr u32 control_interrupt_on_completion = 1 << 5;
constexpr u32 control_immediate = 1 << 6;
constexpr u32 control_direction_in = 1 << 16;

constexpr u32 code_success = 1;
constexpr u32 code_stall = 6;
constexpr u32 code_short_packet = 13;

struct TRB {
    u64 parameter;
    u32 status;
    u32 control;
};
static_assert(sizeof(TRB) == 16);

constexpr size_t ring_entries = page_size / sizeof(TRB);

// What goes round and is filled in here: the last entry links back to the first.
class Ring {
public:
    ErrorOr<void> create()
    {
        m_memory = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto* entries = this->entries();
        entries[ring_entries - 1].parameter = m_memory.get();
        entries[ring_entries - 1].control = (trb_link << 10) | control_toggle_cycle;
        return {};
    }

    [[nodiscard]] PhysicalAddress address() const { return m_memory; }
    [[nodiscard]] bool cycle() const { return m_cycle; }
    [[nodiscard]] PhysicalAddress enqueue_address() const { return m_memory.offset(m_enqueue * sizeof(TRB)); }

    // Returns where the block went. The cycle bit goes last, since that's what hands it over.
    u64 push(u64 parameter, u32 status, u32 control)
    {
        auto* entries = this->entries();
        u64 const where = enqueue_address().get();
        auto& entry = entries[m_enqueue];
        entry.parameter = parameter;
        entry.status = status;
        Arch::memory_barrier();
        entry.control = control | (m_cycle ? control_cycle : 0);
        ++m_enqueue;
        if (m_enqueue == ring_entries - 1) {
            auto& link = entries[m_enqueue];
            Arch::memory_barrier();
            link.control = (link.control & ~control_cycle) | (m_cycle ? control_cycle : 0);
            m_cycle = !m_cycle;
            m_enqueue = 0;
        }
        return where;
    }

    // After it's been reset: start again from a clean state, with the cycle it will now be told about.
    void clear()
    {
        __builtin_memset(entries(), 0, (ring_entries - 1) * sizeof(TRB));
        auto& link = entries()[ring_entries - 1];
        link.control = (trb_link << 10) | control_toggle_cycle;
        m_enqueue = 0;
        m_cycle = true;
    }

private:
    [[nodiscard]] TRB* entries() const { return reinterpret_cast<TRB*>(physical_to_virtual(m_memory)); }

    PhysicalAddress m_memory;
    size_t m_enqueue { 0 };
    bool m_cycle { true };
};

class XHCIDevice;
class Controller;

class XHCIEndpoint final : public Endpoint {
public:
    u8 dci { 0 };
    EndpointDescription description;
    Ring ring;
    Mutex lock;
    // Each transfer that finishes: how it went.
    Atomic<u32> finished { 0 };
    Atomic<u32> code { 0 };
    Atomic<u32> residue { 0 };
    WaitQueue done;
    // For interrupts.
    Device::ReportCallback callback { nullptr };
    void* callback_context { nullptr };
    PhysicalAddress reports;
    size_t report_size { 0 };
    u32 report_slots { 0 };
    u32 next_report { 0 };
};

class XHCIDevice final : public Device {
public:
    // Where it is: the port on the controller it comes by way of, and the ports of any hubs after that.
    struct Location {
        u8 root_port { 0 };
        u32 route { 0 };
        u8 depth { 0 };
        u8 speed { 0 };
        u8 parent_slot { 0 }; // For a slow device on a fast hub, which translates for it.
        u8 parent_port { 0 };
    };

    XHCIDevice(Controller& controller, u8 slot, Location const& location)
        : m_controller(controller)
        , m_slot(slot)
        , m_port(location.root_port)
        , m_speed(location.speed)
        , m_location(location)
    {
    }

    void rescan_hub();

    ErrorOr<void> initialize(u16 first_max_packet);
    ErrorOr<void> bring_up();

    ErrorOr<size_t> control(u8 request_type, u8 request, u16 value, u16 index, Bytes data) override;
    ErrorOr<Endpoint*> open_endpoint(EndpointDescription const&) override;
    ErrorOr<size_t> bulk(Endpoint&, PhysicalAddress buffer, size_t length) override;
    ErrorOr<void> poll_interrupts(Endpoint&, size_t report_size, ReportCallback, void* context) override;
    ErrorOr<void> clear_stall(Endpoint&) override;

    void handle_transfer_event(u8 dci, u32 code, u32 residue);

private:
    [[nodiscard]] u8* input_context(size_t index) const;
    [[nodiscard]] u32* slot_context_in_input() const { return reinterpret_cast<u32*>(input_context(1)); }
    [[nodiscard]] u32* endpoint_context_in_input(u8 dci) const { return reinterpret_cast<u32*>(input_context(1 + dci)); }
    void clear_input_context();
    void configure_slot_context(u8 highest_dci);
    ErrorOr<size_t> wait_for_transfer(XHCIEndpoint&, u32 finished_before, size_t length);
    ErrorOr<bool> attach_hub(InterfaceDescription const&);
    ErrorOr<void> attach_hub_port(u8 port);

    Controller& m_controller;
    u8 m_slot;
    u8 m_port;
    u8 m_speed;
    PhysicalAddress m_device_context;
    PhysicalAddress m_input_context;
    PhysicalAddress m_control_buffer;
    XHCIEndpoint m_default;
    XHCIEndpoint* m_endpoints[32] {};
    u8 m_highest_dci { 1 };
    Mutex m_control_lock;
    Location m_location;
    bool m_is_hub { false };
    u8 m_hub_ports { 0 };
    u32 m_hub_ports_attached { 0 };
};

class Controller {
public:
    static ErrorOr<Controller*> create(PCI::Device const&);
    // Whatever the controller has to say, now. Its own thread does this, unless it can't.
    void drain_events();

    ErrorOr<u64> run_command(u64 parameter, u32 status, u32 control, u8* slot_out = nullptr);
    void ring_doorbell(u8 slot, u32 target) { mmio_write32(m_doorbells + static_cast<FlatPtr>(slot) * 4, target); }
    [[nodiscard]] size_t context_size() const { return m_context_size; }
    void set_device_context(u8 slot, PhysicalAddress address) { reinterpret_cast<u64*>(physical_to_virtual(m_device_contexts))[slot] = address.get(); }

    void scan_ports();
    ErrorOr<void> attach_device(XHCIDevice::Location const&);
    void hub_changed(XHCIDevice&);

private:
    Controller(FlatPtr capabilities, FlatPtr operational, FlatPtr runtime, FlatPtr doorbells, u32 ports)
        : m_capabilities(capabilities)
        , m_operational(operational)
        , m_runtime(runtime)
        , m_doorbells(doorbells)
        , m_port_count(ports)
    {
    }

    ErrorOr<void> start();
    void take_from_firmware();
    void handle_interrupt();
    void event_loop();
    void port_loop();
    void process_event(TRB const&);
    ErrorOr<void> attach_port(u8 port);
    ErrorOr<void> reset_port(u8 port);
    [[nodiscard]] FlatPtr port_register(u8 port) const { return m_operational + op_port_status + (port - 1) * 0x10; }
    void write_dequeue(u64 value);

    FlatPtr m_capabilities;
    FlatPtr m_operational;
    FlatPtr m_runtime;
    FlatPtr m_doorbells;
    u32 m_port_count;
    size_t m_context_size { 32 };
    u32 m_slot_count { 0 };

    PhysicalAddress m_device_contexts;
    Ring m_command_ring;
    Mutex m_command_lock;
    Atomic<u32> m_command_seen { 0 };
    u32 m_command_code { 0 };
    u8 m_command_slot { 0 };
    WaitQueue m_command_done;

    PhysicalAddress m_event_ring;
    PhysicalAddress m_event_table;
    size_t m_event_index { 0 };
    bool m_event_cycle { true };
    Atomic<bool> m_pending { false };
    WaitQueue m_wait;

    Atomic<u32> m_ports_changed { 0 };
    WaitQueue m_port_wait;
    XHCIDevice* m_slots[256] {};
    XHCIDevice* m_devices_on_ports[64] {};
    // Hubs that have said something changed, as bits by slot, for the thread that deals with ports.
    Atomic<u64> m_hubs_changed[4] {};
};

// A command's or transfer's wait: how long, at the most, anything here takes.
constexpr u64 command_timeout_milliseconds = 5000;

template<typename Condition>
[[nodiscard]] bool wait_with_deadline(WaitQueue& queue, Condition condition, u64 milliseconds)
{
    u64 const deadline = Time::now() + Time::ticks_from_milliseconds(milliseconds);
    while (!condition()) {
        if (Time::now() >= deadline)
            return condition();
        (void)queue.wait_until_interruptible(condition, deadline);
    }
    return true;
}

template<typename Condition>
[[nodiscard]] bool poll_until(Condition condition, u64 milliseconds)
{
    u64 const deadline = Time::now() + Time::ticks_from_milliseconds(milliseconds);
    while (!condition()) {
        if (Time::now() >= deadline)
            return false;
        Arch::pause();
    }
    return true;
}

void write_64(FlatPtr address, u64 value)
{
    mmio_write32(address, static_cast<u32>(value));
    mmio_write32(address + 4, static_cast<u32>(value >> 32));
}

// ---- A device.

u8* XHCIDevice::input_context(size_t index) const
{
    return physical_to_virtual(m_input_context) + index * m_controller.context_size();
}

void XHCIDevice::clear_input_context()
{
    __builtin_memset(physical_to_virtual(m_input_context), 0, page_size);
}

void XHCIDevice::configure_slot_context(u8 highest_dci)
{
    auto* slot = slot_context_in_input();
    constexpr u32 slot_is_hub = 1u << 26;
    slot[0] = (static_cast<u32>(highest_dci) << 27) | (static_cast<u32>(m_speed) << 20) | m_location.route | (m_is_hub ? slot_is_hub : 0);
    slot[1] = (static_cast<u32>(m_port) << 16) | (static_cast<u32>(m_hub_ports) << 24);
    slot[2] = static_cast<u32>(m_location.parent_slot) | (static_cast<u32>(m_location.parent_port) << 8);
}

ErrorOr<void> XHCIDevice::initialize(u16 first_max_packet)
{
    m_device_context = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    m_input_context = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    m_control_buffer = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    TRY(m_default.ring.create());
    m_default.dci = 1;
    m_endpoints[1] = &m_default;
    m_controller.set_device_context(m_slot, m_device_context);

    clear_input_context();
    // Which contexts this is about: the slot, and the first endpoint.
    auto* control = reinterpret_cast<u32*>(input_context(0));
    control[1] = (1 << 0) | (1 << 1);
    configure_slot_context(1);
    auto* endpoint = endpoint_context_in_input(1);
    endpoint[1] = (3 << 1) | (4 << 3) | (static_cast<u32>(first_max_packet) << 16); // Three tries; control.
    u64 const ring = m_default.ring.address().get() | 1;
    endpoint[2] = static_cast<u32>(ring);
    endpoint[3] = static_cast<u32>(ring >> 32);
    endpoint[4] = 8; // How long a transfer tends to be.
    TRY(m_controller.run_command(m_input_context.get(), 0, (trb_address_device << 10) | (static_cast<u32>(m_slot) << 24)));
    return {};
}

ErrorOr<size_t> XHCIDevice::wait_for_transfer(XHCIEndpoint& endpoint, u32 finished_before, size_t length)
{
    // In a panic, the thread that reads events has stopped with everything else.
    if (is_panicking()) {
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(5'000);
        while (endpoint.finished.load() == finished_before && Time::now() < deadline) {
            m_controller.drain_events();
            Arch::pause();
        }
    }
    bool const finished = is_panicking() ? endpoint.finished.load() != finished_before : wait_with_deadline(endpoint.done, [&] { return endpoint.finished.load() != finished_before; }, command_timeout_milliseconds);
    if (!finished)
        return Error::from_errno(ETIMEDOUT);
    u32 const code = endpoint.code.load();
    if (code == code_stall)
        return Error::from_errno(EPIPE);
    if (code != code_success && code != code_short_packet)
        return Error::from_errno(EIO);
    u32 const residue = endpoint.residue.load();
    return length >= residue ? length - residue : 0;
}

ErrorOr<size_t> XHCIDevice::control(u8 request_type, u8 request, u16 value, u16 index, Bytes data)
{
    MutexLocker locker(m_control_lock);
    if (data.size() > page_size)
        return Error::from_errno(EINVAL);
    bool const in = request_type & 0x80;
    if (!in && !data.is_empty())
        __builtin_memcpy(physical_to_virtual(m_control_buffer), data.data(), data.size());

    u64 const setup = static_cast<u64>(request_type) | (static_cast<u64>(request) << 8) | (static_cast<u64>(value) << 16)
        | (static_cast<u64>(index) << 32) | (static_cast<u64>(data.size()) << 48);
    // Which way the data goes, for the setup stage: 0 none, 2 out, 3 in.
    u32 const transfer_type = data.is_empty() ? 0 : (in ? 3 : 2);
    u32 const finished_before = m_default.finished.load();
    m_default.ring.push(setup, 8, (trb_setup << 10) | control_immediate | (transfer_type << 16));
    if (!data.is_empty())
        m_default.ring.push(m_control_buffer.get(), static_cast<u32>(data.size()), (trb_data << 10) | (in ? control_direction_in : 0));
    // The status stage goes the other way, and finishes it.
    m_default.ring.push(0, 0, (trb_status << 10) | control_interrupt_on_completion | ((in && !data.is_empty()) ? 0 : control_direction_in));
    Arch::device_barrier();
    m_controller.ring_doorbell(m_slot, 1);
    size_t const transferred = TRY(wait_for_transfer(m_default, finished_before, data.size()));
    if (in && !data.is_empty())
        __builtin_memcpy(data.data(), physical_to_virtual(m_control_buffer), min(transferred, data.size()));
    return transferred;
}

ErrorOr<Endpoint*> XHCIDevice::open_endpoint(EndpointDescription const& description)
{
    auto* endpoint = new (Base::nothrow) XHCIEndpoint;
    if (!endpoint)
        return Error::from_errno(ENOMEM);
    TRY(endpoint->ring.create());
    endpoint->description = description;
    u8 const number = description.address & 0x0f;
    endpoint->dci = static_cast<u8>(number * 2 + (description.is_in() ? 1 : 0));
    m_endpoints[endpoint->dci] = endpoint;
    if (endpoint->dci > m_highest_dci)
        m_highest_dci = endpoint->dci;

    clear_input_context();
    auto* control = reinterpret_cast<u32*>(input_context(0));
    control[1] = (1 << 0) | (1u << endpoint->dci);
    configure_slot_context(m_highest_dci);
    auto* context = endpoint_context_in_input(endpoint->dci);
    // Its type: the kind, four more if it's coming in.
    u32 const type = static_cast<u32>(description.kind) + (description.is_in() ? 4 : 0);
    context[1] = (3 << 1) | (type << 3) | (static_cast<u32>(description.max_packet) << 16);
    // How often: by powers of two of 125 µs, which isn't how slower devices say it.
    u32 interval = 0;
    if (description.kind == EndpointKind::Interrupt) {
        if (m_speed >= 3) {
            interval = description.interval ? description.interval - 1u : 0;
        } else {
            u32 const microframes = (description.interval ? description.interval : 1u) * 8;
            while ((1u << (interval + 1)) <= microframes && interval < 10)
                ++interval;
        }
    }
    context[0] = interval << 16;
    u64 const ring = endpoint->ring.address().get() | 1;
    context[2] = static_cast<u32>(ring);
    context[3] = static_cast<u32>(ring >> 32);
    context[4] = description.kind == EndpointKind::Interrupt ? description.max_packet : 3072;
    if (description.kind == EndpointKind::Interrupt)
        context[4] |= static_cast<u32>(description.max_packet) << 16;
    TRY(m_controller.run_command(m_input_context.get(), 0, (trb_configure_endpoint << 10) | (static_cast<u32>(m_slot) << 24)));
    return endpoint;
}

ErrorOr<size_t> XHCIDevice::bulk(Endpoint& opened, PhysicalAddress buffer, size_t length)
{
    auto& endpoint = static_cast<XHCIEndpoint&>(opened);
    MutexLocker locker(endpoint.lock);
    u32 const finished_before = endpoint.finished.load();
    endpoint.ring.push(buffer.get(), static_cast<u32>(length), (trb_normal << 10) | control_interrupt_on_completion | control_interrupt_short);
    Arch::device_barrier();
    m_controller.ring_doorbell(m_slot, endpoint.dci);
    return wait_for_transfer(endpoint, finished_before, length);
}

ErrorOr<void> XHCIDevice::poll_interrupts(Endpoint& opened, size_t report_size, ReportCallback callback, void* context)
{
    auto& endpoint = static_cast<XHCIEndpoint&>(opened);
    endpoint.callback = callback;
    endpoint.callback_context = context;
    endpoint.report_size = report_size;
    endpoint.report_slots = static_cast<u32>(page_size / report_size);
    if (endpoint.report_slots > 16)
        endpoint.report_slots = 16;
    endpoint.reports = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    for (u32 i = 0; i < endpoint.report_slots; ++i)
        endpoint.ring.push(endpoint.reports.get() + i * report_size, static_cast<u32>(report_size), (trb_normal << 10) | control_interrupt_on_completion | control_interrupt_short);
    Arch::device_barrier();
    m_controller.ring_doorbell(m_slot, endpoint.dci);
    return {};
}

ErrorOr<void> XHCIDevice::clear_stall(Endpoint& opened)
{
    auto& endpoint = static_cast<XHCIEndpoint&>(opened);
    TRY(m_controller.run_command(0, 0, (trb_reset_endpoint << 10) | (static_cast<u32>(endpoint.dci) << 16) | (static_cast<u32>(m_slot) << 24)));
    endpoint.ring.clear();
    u64 const ring = endpoint.ring.address().get() | 1;
    TRY(m_controller.run_command(ring, 0, (trb_set_dequeue << 10) | (static_cast<u32>(endpoint.dci) << 16) | (static_cast<u32>(m_slot) << 24)));
    (void)TRY(control(0x02, request_clear_feature, 0, endpoint.description.address, {}));
    return {};
}

void XHCIDevice::handle_transfer_event(u8 dci, u32 code, u32 residue)
{
    auto* endpoint = dci < 32 ? m_endpoints[dci] : nullptr;
    if (!endpoint)
        return;
    if (endpoint->callback) {
        if (code == code_success || code == code_short_packet) {
            auto const* report = physical_to_virtual(endpoint->reports.offset(endpoint->next_report * endpoint->report_size));
            size_t const length = endpoint->report_size >= residue ? endpoint->report_size - residue : 0;
            endpoint->callback(endpoint->callback_context, { report, length });
        }
        // And put it back on the end, for the next.
        endpoint->ring.push(endpoint->reports.get() + endpoint->next_report * endpoint->report_size, static_cast<u32>(endpoint->report_size), (trb_normal << 10) | control_interrupt_on_completion | control_interrupt_short);
        endpoint->next_report = (endpoint->next_report + 1) % endpoint->report_slots;
        Arch::device_barrier();
        m_controller.ring_doorbell(m_slot, dci);
        return;
    }
    endpoint->code.store(code);
    endpoint->residue.store(residue);
    endpoint->finished.fetch_add(1);
    endpoint->done.wake_all();
}

// Reads what it has to say for itself, and finds it a driver.
ErrorOr<void> XHCIDevice::bring_up()
{
    u8 device_bytes[18];
    // The first eight say how big a packet can be, which the rest depend on.
    (void)TRY(control(0x80, request_get_descriptor, descriptor_device << 8, 0, { device_bytes, 8 }));
    u16 const max_packet = m_speed >= 4 ? static_cast<u16>(1u << device_bytes[7]) : device_bytes[7];
    if (max_packet != 0) {
        clear_input_context();
        auto* control_context = reinterpret_cast<u32*>(input_context(0));
        control_context[1] = 1 << 1;
        auto* endpoint = endpoint_context_in_input(1);
        endpoint[1] = (3 << 1) | (4 << 3) | (static_cast<u32>(max_packet) << 16);
        TRY(m_controller.run_command(m_input_context.get(), 0, (trb_evaluate_context << 10) | (static_cast<u32>(m_slot) << 24)));
    }
    (void)TRY(control(0x80, request_get_descriptor, descriptor_device << 8, 0, { device_bytes, sizeof(device_bytes) }));
    u16 const vendor = static_cast<u16>(device_bytes[8] | (device_bytes[9] << 8));
    u16 const product = static_cast<u16>(device_bytes[10] | (device_bytes[11] << 8));

    u8 header[9];
    (void)TRY(control(0x80, request_get_descriptor, descriptor_configuration << 8, 0, { header, sizeof(header) }));
    u16 const total = min<u16>(static_cast<u16>(header[2] | (header[3] << 8)), page_size);
    u8 const configuration_value = header[5];
    auto* configuration = static_cast<u8*>(kmalloc(total));
    if (!configuration)
        return Error::from_errno(ENOMEM);
    (void)TRY(control(0x80, request_get_descriptor, descriptor_configuration << 8, 0, { configuration, total }));

    // It's only configured after all of that's read, and before anything of its endpoints is used.
    (void)TRY(control(0x00, request_set_configuration, configuration_value, 0, {}));

    InterfaceDescription interfaces[8];
    size_t interface_count = 0;
    InterfaceDescription* current = nullptr;
    for (size_t at = 0; at + 2 <= total && configuration[at] >= 2; at += configuration[at]) {
        u8 const length = configuration[at];
        u8 const type = configuration[at + 1];
        if (at + length > total)
            break;
        u8 const* entry = configuration + at;
        if (type == descriptor_interface && length >= 9) {
            current = nullptr;
            // Alternate settings aren't used.
            if (entry[3] == 0 && interface_count < 8) {
                current = &interfaces[interface_count++];
                current->number = entry[2];
                current->interface_class = entry[5];
                current->subclass = entry[6];
                current->protocol = entry[7];
            }
        } else if (type == descriptor_hid && length >= 9 && current) {
            current->report_descriptor_length = static_cast<u16>(entry[7] | (entry[8] << 8));
        } else if (type == descriptor_endpoint && length >= 7 && current && current->endpoint_count < 4) {
            auto& endpoint = current->endpoints[current->endpoint_count++];
            endpoint.address = entry[2];
            endpoint.kind = static_cast<EndpointKind>(entry[3] & 3);
            endpoint.max_packet = static_cast<u16>((entry[4] | (entry[5] << 8)) & 0x7ff);
            endpoint.interval = entry[6];
        }
    }
    kfree(configuration);

    bool any = false;
    for (size_t i = 0; i < interface_count; ++i) {
        auto const& interface = interfaces[i];
        ErrorOr<bool> attached = false;
        if (interface.interface_class == class_hub)
            attached = attach_hub(interface);
        else if (interface.interface_class == class_hid)
            attached = attach_hid(*this, interface);
        else if (interface.interface_class == class_mass_storage)
            attached = attach_mass_storage(*this, interface);
        if (attached.is_error())
            dmesgln("USB: port {}, interface {}: {}", m_port, interface.number, attached.error());
        else if (attached.value())
            any = true;
    }
    if (!any)
        dmesgln("USB: port {}: {:04x}:{:04x}, nothing here can use it", m_port, vendor, product);
    return {};
}

// ---- Hubs: devices with more ports on them, which this reaches by way of requests to the hub.

namespace {

constexpr u8 request_get_status = 0;
constexpr u8 request_set_feature = 3;
constexpr u8 to_hub_port = 0x23;
constexpr u8 from_hub_port = 0xa3;
constexpr u8 from_hub_class = 0xa0;
constexpr u8 descriptor_hub = 0x29;
constexpr u8 descriptor_super_speed_hub = 0x2a;
constexpr u16 feature_port_reset = 4;
constexpr u16 feature_port_power = 8;
constexpr u16 feature_clear_connection = 16;
constexpr u16 feature_clear_reset = 20;
constexpr u32 hub_port_connected = 1 << 0;
constexpr u32 hub_port_enabled = 1 << 1;
constexpr u32 hub_port_low_speed = 1 << 9;
constexpr u32 hub_port_high_speed = 1 << 10;
constexpr u32 hub_port_reset_changed = 1 << 20;

}

ErrorOr<bool> XHCIDevice::attach_hub(InterfaceDescription const& interface)
{
    // Five levels are all there's room for in the address of what's on one.
    if (m_location.depth >= 5)
        return false;
    bool const super_speed = m_speed >= 4;
    u8 descriptor[16];
    (void)TRY(control(from_hub_class, request_get_descriptor, (super_speed ? descriptor_super_speed_hub : descriptor_hub) << 8, 0, { descriptor, sizeof(descriptor) }));
    m_hub_ports = min<u8>(descriptor[2], 15);
    m_is_hub = true;
    Optional<EndpointDescription> in;
    for (u8 i = 0; i < interface.endpoint_count; ++i) {
        if (interface.endpoints[i].kind == EndpointKind::Interrupt && interface.endpoints[i].is_in())
            in = interface.endpoints[i];
    }
    if (!in.has_value())
        return false;
    // This also tells the controller it's a hub, as that's what's in the slot's context now.
    auto* endpoint = TRY(open_endpoint(in.value()));
    for (u8 port = 1; port <= m_hub_ports; ++port)
        (void)TRY(control(to_hub_port, request_set_feature, feature_port_power, port, {}));
    // In units of 2 ms: how long it says its ports need.
    u32 const power_on_ms = max<u32>(descriptor[5], 50u) * 2;
    Time::wait_until(Time::now() + Time::ticks_from_milliseconds(power_on_ms));
    dmesgln("USB: a hub with {} ports", m_hub_ports);
    rescan_hub();
    TRY(poll_interrupts(*endpoint, in->max_packet ? min<size_t>(in->max_packet, 8) : 1, [](void* context, ReadonlyBytes) {
        auto& hub = *static_cast<XHCIDevice*>(context);
        hub.m_controller.hub_changed(hub);
    }, this));
    return true;
}

void XHCIDevice::rescan_hub()
{
    for (u8 port = 1; port <= m_hub_ports; ++port) {
        u8 status_bytes[4] {};
        if (control(from_hub_port, request_get_status, 0, port, { status_bytes, sizeof(status_bytes) }).is_error())
            continue;
        u32 status;
        __builtin_memcpy(&status, status_bytes, sizeof(status));
        // A change it has told about stays until it's told it has been heard.
        (void)control(0x23, request_clear_feature, feature_clear_connection, port, {});
        u32 const bit = 1u << port;
        bool const connected = status & hub_port_connected;
        bool const attached = m_hub_ports_attached & bit;
        if (connected && !attached) {
            if (auto result = attach_hub_port(port); result.is_error())
                dmesgln("USB: hub port {}: {}", port, result.error());
            else
                m_hub_ports_attached |= bit;
        } else if (!connected && attached) {
            dmesgln("USB: hub port {}: removed", port);
            m_hub_ports_attached &= ~bit;
        }
    }
}

ErrorOr<void> XHCIDevice::attach_hub_port(u8 port)
{
    (void)TRY(control(to_hub_port, request_set_feature, feature_port_reset, port, {}));
    u32 status = 0;
    u64 const deadline = Time::now() + Time::ticks_from_milliseconds(500);
    for (;;) {
        u8 status_bytes[4] {};
        (void)TRY(control(from_hub_port, request_get_status, 0, port, { status_bytes, sizeof(status_bytes) }));
        __builtin_memcpy(&status, status_bytes, sizeof(status));
        if (status & hub_port_reset_changed)
            break;
        if (Time::now() > deadline)
            return Error::from_string_literal("the hub's port didn't reset");
        (void)Thread::sleep_for_microseconds(10'000);
    }
    (void)TRY(control(0x23, request_clear_feature, feature_clear_reset, port, {}));
    if (!(status & hub_port_enabled))
        return Error::from_string_literal("the hub's port isn't enabled");

    Location child;
    child.root_port = m_location.root_port;
    child.route = m_location.route | (static_cast<u32>(port) << (4 * m_location.depth));
    child.depth = static_cast<u8>(m_location.depth + 1);
    if (m_speed >= 4)
        child.speed = m_speed;
    else if (status & hub_port_low_speed)
        child.speed = 2;
    else if (status & hub_port_high_speed)
        child.speed = 3;
    else
        child.speed = 1;
    // A slow device on a high-speed hub goes through the hub's translator.
    if (m_speed == 3 && child.speed < 3) {
        child.parent_slot = m_slot;
        child.parent_port = port;
    } else {
        child.parent_slot = m_location.parent_slot;
        child.parent_port = m_location.parent_port;
    }
    return m_controller.attach_device(child);
}

// ---- The controller.

void Controller::take_from_firmware()
{
    u32 const parameters = mmio_read32(m_capabilities + cap_capability_1);
    FlatPtr at = m_capabilities + (static_cast<FlatPtr>(parameters >> 16) << 2);
    for (int guard = 0; (parameters >> 16) && guard < 64; ++guard) {
        u32 const word = mmio_read32(at);
        // The first of these is whether the firmware's in charge of it, as keyboards work before there's
        // an operating system to do it.
        if ((word & 0xff) == 1) {
            constexpr u32 bios_owned = 1 << 16;
            constexpr u32 os_owned = 1 << 24;
            mmio_write32(at, word | os_owned);
            if (!poll_until([&] { return !(mmio_read32(at) & bios_owned); }, 1000))
                dmesgln("xHCI: the firmware won't let go of it");
            // No more interrupts to the firmware for what happens from here.
            mmio_write32(at + 4, mmio_read32(at + 4) & 0x1f00e);
            return;
        }
        u32 const next = (word >> 8) & 0xff;
        if (next == 0)
            return;
        at += static_cast<FlatPtr>(next) << 2;
    }
}

ErrorOr<Controller*> Controller::create(PCI::Device const& pci)
{
    auto const bar = pci.bar(0);
    if (!bar.is_memory || bar.address == 0)
        return Error::from_string_literal("no registers");
    pci.enable_memory_and_bus_mastering();
    FlatPtr const capabilities = TRY(memory_manager().map_mmio(PhysicalAddress(bar.address), bar.size ? bar.size : 64 * KiB));
    FlatPtr const operational = capabilities + mmio_read8(capabilities + cap_length);
    FlatPtr const runtime = capabilities + (mmio_read32(capabilities + cap_runtime_offset) & ~0x1fu);
    FlatPtr const doorbells = capabilities + (mmio_read32(capabilities + cap_doorbell_offset) & ~0x3u);
    u32 const structural = mmio_read32(capabilities + cap_structural_1);
    u32 const ports = (structural >> 24) & 0xff;

    auto* controller = new (Base::nothrow) Controller(capabilities, operational, runtime, doorbells, ports);
    if (!controller)
        return Error::from_errno(ENOMEM);
    controller->m_slot_count = min<u32>(structural & 0xff, 255);
    controller->m_context_size = (mmio_read32(capabilities + cap_capability_1) & (1 << 2)) ? 64 : 32;
    controller->take_from_firmware();
    TRY(controller->start());

    auto const interrupt = pci.interrupt();
    if (!interrupt.has_value())
        return Error::from_errno(ENODEV);
    register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<Controller*>(context)->handle_interrupt(); }, controller);
    Arch::enable_interrupt(interrupt.value());
    auto events = TRY(Thread::create("xhci-events"_sv, [](void* context) { static_cast<Controller*>(context)->event_loop(); }, controller, { .qos = QoS::UserInteractive }));
    events->start();
    FlatPtr const interrupter = runtime + interrupter_0;
    mmio_write32(interrupter + interrupter_management, management_enable | management_pending);
    mmio_write32(operational + op_command, mmio_read32(operational + op_command) | command_run | command_interrupts);
    if (!poll_until([&] { return !(mmio_read32(operational + op_status) & status_halted); }, 100))
        return Error::from_string_literal("it won't start");
    return controller;
}

ErrorOr<void> Controller::start()
{
    // Stopped, then reset.
    mmio_write32(m_operational + op_command, mmio_read32(m_operational + op_command) & ~command_run);
    if (!poll_until([&] { return mmio_read32(m_operational + op_status) & status_halted; }, 100))
        return Error::from_string_literal("it won't stop");
    mmio_write32(m_operational + op_command, command_reset);
    if (!poll_until([&] { return !(mmio_read32(m_operational + op_command) & command_reset) && !(mmio_read32(m_operational + op_status) & status_not_ready); }, 1000))
        return Error::from_string_literal("it won't reset");

    mmio_write32(m_operational + op_configure, m_slot_count);
    m_device_contexts = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    // Some memory it keeps for itself, on real machines, and is told about in the first entry.
    u32 const structural_2 = mmio_read32(m_capabilities + cap_structural_2);
    u32 const scratchpads = ((structural_2 >> 21) & 0x1f) << 5 | ((structural_2 >> 27) & 0x1f);
    if (scratchpads > 0) {
        auto const array = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto* entries = reinterpret_cast<u64*>(physical_to_virtual(array));
        for (u32 i = 0; i < scratchpads && i < page_size / sizeof(u64); ++i)
            entries[i] = TRY(physical_memory().allocate_zeroed_page(PageUse::General)).get();
        reinterpret_cast<u64*>(physical_to_virtual(m_device_contexts))[0] = array.get();
    }
    write_64(m_operational + op_device_contexts, m_device_contexts.get());

    TRY(m_command_ring.create());
    write_64(m_operational + op_command_ring, m_command_ring.address().get() | 1);

    m_event_ring = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    m_event_table = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    auto* table = reinterpret_cast<u64*>(physical_to_virtual(m_event_table));
    table[0] = m_event_ring.get();
    table[1] = ring_entries;
    FlatPtr const interrupter = m_runtime + interrupter_0;
    mmio_write32(interrupter + interrupter_table_size, 1);
    write_64(interrupter + interrupter_dequeue, m_event_ring.get());
    write_64(interrupter + interrupter_table, m_event_table.get());
    // About a millisecond's worth of gathering them up.
    mmio_write32(interrupter + interrupter_moderation, 4000);
    return {};
}

void Controller::handle_interrupt()
{
    FlatPtr const interrupter = m_runtime + interrupter_0;
    u32 const management = mmio_read32(interrupter + interrupter_management);
    if (!(management & management_pending))
        return;
    // These are cleared by writing them.
    mmio_write32(m_operational + op_status, 1 << 3);
    mmio_write32(interrupter + interrupter_management, management | management_pending);
    m_pending.store(true);
    m_wait.wake_all();
}

void Controller::write_dequeue(u64 value)
{
    write_64(m_runtime + interrupter_0 + interrupter_dequeue, value | dequeue_busy);
}

void Controller::process_event(TRB const& event)
{
    u32 const type = (event.control >> 10) & 0x3f;
    switch (type) {
    case trb_event_command:
        m_command_code = event.status >> 24;
        m_command_slot = static_cast<u8>(event.control >> 24);
        m_command_seen.fetch_add(1);
        m_command_done.wake_all();
        break;
    case trb_event_transfer: {
        u8 const slot = static_cast<u8>(event.control >> 24);
        u8 const dci = (event.control >> 16) & 0x1f;
        if (auto* device = m_slots[slot])
            device->handle_transfer_event(dci, event.status >> 24, event.status & 0xff'ffff);
        break;
    }
    case trb_event_port: {
        u32 const port = static_cast<u32>(event.parameter >> 24) & 0xff;
        if (port >= 1 && port <= 32) {
            m_ports_changed.fetch_or(1u << (port - 1));
            m_port_wait.wake_all();
        }
        break;
    }
    default:
        break;
    }
}

void Controller::drain_events()
{
    auto const* ring = reinterpret_cast<TRB const volatile*>(physical_to_virtual(m_event_ring));
    bool any = false;
    for (;;) {
        auto const& entry = ring[m_event_index];
        if (static_cast<bool>(entry.control & control_cycle) != m_event_cycle)
            break;
        Arch::memory_barrier();
        TRB const event { entry.parameter, entry.status, entry.control };
        ++m_event_index;
        if (m_event_index == ring_entries) {
            m_event_index = 0;
            m_event_cycle = !m_event_cycle;
        }
        process_event(event);
        any = true;
    }
    if (any)
        write_dequeue(m_event_ring.get() + m_event_index * sizeof(TRB));
}

void Controller::event_loop()
{
    for (;;) {
        // Now and then regardless, for a controller whose interrupts don't reach here for some reason.
        (void)wait_with_deadline(m_wait, [this] { return m_pending.load(); }, 100);
        m_pending.store(false);
        drain_events();
    }
}

ErrorOr<u64> Controller::run_command(u64 parameter, u32 status, u32 control, u8* slot_out)
{
    MutexLocker locker(m_command_lock);
    u32 const seen = m_command_seen.load();
    m_command_ring.push(parameter, status, control);
    Arch::device_barrier();
    ring_doorbell(0, 0);
    if (!wait_with_deadline(m_command_done, [&] { return m_command_seen.load() != seen; }, command_timeout_milliseconds))
        return Error::from_string_literal("the controller didn't answer a command");
    if (m_command_code != code_success) {
        dmesgln("xHCI: command {} failed: {}", (control >> 10) & 0x3f, m_command_code);
        return Error::from_errno(EIO);
    }
    if (slot_out)
        *slot_out = m_command_slot;
    return 0;
}

ErrorOr<void> Controller::reset_port(u8 port)
{
    FlatPtr const reg = port_register(port);
    u32 const value = mmio_read32(reg);
    // What's written as 1 to clear shouldn't be written as it was read, nor "enabled", which disables it.
    mmio_write32(reg, (value & ~(port_enabled | port_changes)) | port_reset);
    if (!poll_until([&] { return mmio_read32(reg) & port_reset_changed; }, 500))
        return Error::from_string_literal("the port didn't reset");
    u32 const after = mmio_read32(reg);
    mmio_write32(reg, (after & ~(port_enabled | port_changes)) | port_reset_changed);
    if (!(mmio_read32(reg) & port_enabled))
        return Error::from_string_literal("the port isn't enabled");
    return {};
}

ErrorOr<void> Controller::attach_port(u8 port)
{
    FlatPtr const reg = port_register(port);
    if (!(mmio_read32(reg) & port_enabled))
        TRY(reset_port(port));
    XHCIDevice::Location location;
    location.root_port = port;
    location.speed = (mmio_read32(reg) >> 10) & 0xf;
    return attach_device(location);
}

ErrorOr<void> Controller::attach_device(XHCIDevice::Location const& location)
{
    u16 first_max_packet = 8;
    if (location.speed == 3)
        first_max_packet = 64;
    else if (location.speed >= 4)
        first_max_packet = 512;

    u8 slot = 0;
    TRY(run_command(0, 0, trb_enable_slot << 10, &slot));
    auto* device = new (Base::nothrow) XHCIDevice(*this, slot, location);
    if (!device)
        return Error::from_errno(ENOMEM);
    m_slots[slot] = device;
    TRY(device->initialize(first_max_packet));
    TRY(device->bring_up());
    if (location.route == 0 && location.root_port < sizeof(m_devices_on_ports) / sizeof(m_devices_on_ports[0]))
        m_devices_on_ports[location.root_port] = device;
    return {};
}

void Controller::hub_changed(XHCIDevice& hub)
{
    for (u32 slot = 0; slot < 256; ++slot) {
        if (m_slots[slot] == &hub) {
            m_hubs_changed[slot / 64].fetch_or(1ull << (slot % 64));
            m_port_wait.wake_all();
            return;
        }
    }
}

void Controller::scan_ports()
{
    for (u32 port = 1; port <= m_port_count && port < 64; ++port) {
        FlatPtr const reg = port_register(static_cast<u8>(port));
        u32 const value = mmio_read32(reg);
        // Turned on, in case it isn't.
        if (!(value & port_power))
            mmio_write32(reg, (value & ~(port_enabled | port_changes)) | port_power);
    }
    // Something plugged in takes a little while to say so.
    Time::wait_until(Time::now() + Time::ticks_from_milliseconds(100));
    for (u32 port = 1; port <= m_port_count && port < 64; ++port) {
        FlatPtr const reg = port_register(static_cast<u8>(port));
        u32 const value = mmio_read32(reg);
        mmio_write32(reg, (value & ~(port_enabled | port_changes)) | (value & port_changes));
        if (!(value & port_connected))
            continue;
        if (auto attached = attach_port(static_cast<u8>(port)); attached.is_error())
            dmesgln("USB: port {}: {}", port, attached.error());
    }
    auto thread = Thread::create("xhci-ports"_sv, [](void* context) { static_cast<Controller*>(context)->port_loop(); }, this, { .qos = QoS::Utility });
    if (!thread.is_error())
        thread.value()->start();
}

void Controller::port_loop()
{
    for (;;) {
        m_port_wait.wait_until([this] {
            return m_ports_changed.load() != 0 || m_hubs_changed[0].load() != 0 || m_hubs_changed[1].load() != 0 || m_hubs_changed[2].load() != 0 || m_hubs_changed[3].load() != 0;
        });
        for (u32 word = 0; word < 4; ++word) {
            u64 const hubs = m_hubs_changed[word].exchange(0);
            for (u32 bit = 0; bit < 64; ++bit) {
                if ((hubs & (1ull << bit)) && m_slots[word * 64 + bit])
                    m_slots[word * 64 + bit]->rescan_hub();
            }
        }
        u32 const changed = m_ports_changed.exchange(0);
        for (u32 port = 1; port <= m_port_count && port <= 32; ++port) {
            if (!(changed & (1u << (port - 1))))
                continue;
            FlatPtr const reg = port_register(static_cast<u8>(port));
            u32 const value = mmio_read32(reg);
            mmio_write32(reg, (value & ~(port_enabled | port_changes)) | (value & port_changes));
            bool const connected = value & port_connected;
            auto*& device = m_devices_on_ports[port];
            if (connected && !device) {
                // Let it settle.
                (void)Thread::sleep_for_microseconds(100'000);
                if (auto attached = attach_port(static_cast<u8>(port)); attached.is_error())
                    dmesgln("USB: port {}: {}", port, attached.error());
            } else if (!connected && device) {
                dmesgln("USB: port {}: removed", port);
                // What's waiting on it finds out from its next transfer failing. (Its memory stays, as
                // its drivers may still be using it, and nothing that's plugged in often enough matters.)
                for (u32 slot = 0; slot < 256; ++slot) {
                    if (m_slots[slot] == device) {
                        (void)run_command(0, 0, (trb_disable_slot << 10) | (slot << 24));
                        m_slots[slot] = nullptr;
                    }
                }
                device = nullptr;
            }
        }
    }
}

}

void initialize()
{
    for (auto& pci : PCI::devices()) {
        // A serial bus controller; USB; the programming interface that means xHCI.
        if (pci.class_code() != 0x0c || pci.subclass() != 0x03 || pci.read8(0x09) != 0x30)
            continue;
        auto created = Controller::create(pci);
        if (created.is_error()) {
            dmesgln("USB: xHCI at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), created.error());
            continue;
        }
        dmesgln("USB: xHCI at {:02x}:{:02x}.{}", pci.bus(), pci.slot(), pci.function());
        created.value()->scan_ports();
    }
}

}
