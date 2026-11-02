/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Net/NetworkStack.h>
#include <Kernel/Net/Socket.h>
#include <Kernel/Net/TCP.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>

namespace Kernel::Net {

namespace {

constexpr size_t largest_queued_frames = 1024;
constexpr size_t largest_neighbors = 64;
constexpr size_t largest_packets_waiting_per_neighbor = 8;
constexpr u64 neighbor_lifetime_seconds = 300;
constexpr u64 neighbor_retry_milliseconds = 500;
constexpr u32 neighbor_attempts = 4;
constexpr u8 default_time_to_live = 64;

class LoopbackInterface final : public NetworkInterface {
public:
    LoopbackInterface()
        : NetworkInterface(MUST(String::from_view("lo0"_sv)), MACAddress {}, 1500, true)
    {
        address = { 0x7f000001 };
        netmask = { 0xff000000 };
    }

    ErrorOr<void> transmit(ReadonlyBytes frame) override
    {
        NetworkStack::the().frame_received(*this, frame);
        return {};
    }
};

}

struct NetworkStack::QueuedFrame {
    QueuedFrame* next { nullptr };
    NetworkInterface* interface { nullptr };
    size_t size { 0 };
    u8 data[];
};

struct NetworkStack::NeighborEntry {
    struct Waiting {
        u8* data;
        size_t size;
    };

    IPv4Address address;
    NetworkInterface* interface { nullptr };
    MACAddress mac;
    bool resolved { false };
    u64 expires { 0 };
    u64 last_request { 0 };
    u32 attempts { 0 };
    Vector<Waiting> waiting;

    ~NeighborEntry()
    {
        for (auto const& packet : waiting)
            kfree(packet.data);
    }
};

NetworkStack& NetworkStack::the()
{
    static NetworkStack* stack = new NetworkStack;
    return *stack;
}

NetworkStack::NetworkStack() = default;

void NetworkStack::initialize()
{
    auto& stack = the();
    stack.add_interface(adopt_own(*static_cast<NetworkInterface*>(new LoopbackInterface)));
    auto thread = MUST(Thread::create("network"_sv, [](void*) { the().run(); }, nullptr, { .qos = QoS::UserInteractive }));
    thread->start();
    initialize_network_devices();
}

void NetworkStack::add_interface(NonnullOwnPtr<NetworkInterface> interface)
{
    MutexLocker locker(m_lock);
    interface->set_index(static_cast<u32>(m_interfaces.size() + 1));
    dmesgln("Network: {}: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", interface->name(), interface->mac().bytes[0], interface->mac().bytes[1], interface->mac().bytes[2], interface->mac().bytes[3], interface->mac().bytes[4], interface->mac().bytes[5]);
    m_interfaces.append(move(interface));
}

NetworkInterface* NetworkStack::interface_named(StringView name)
{
    for (auto& interface : m_interfaces) {
        if (interface->name() == name)
            return interface.ptr();
    }
    return nullptr;
}

NetworkInterface* NetworkStack::interface_with_index(u32 index)
{
    for (auto& interface : m_interfaces) {
        if (interface->index() == index)
            return interface.ptr();
    }
    return nullptr;
}

void NetworkStack::wake_thread()
{
    m_work_pending.store(true);
    m_wait.wake_all();
}

void NetworkStack::request_timer_tick()
{
    m_timer_tick_wanted.store(true);
    wake_thread();
}

void NetworkStack::defer(DeferredCall& call)
{
    {
        SpinLockGuard guard(m_queue_lock);
        call.next = m_deferred;
        m_deferred = &call;
    }
    wake_thread();
}

void NetworkStack::frame_received(NetworkInterface& interface, ReadonlyBytes frame)
{
    auto* queued = static_cast<QueuedFrame*>(kmalloc(sizeof(QueuedFrame) + frame.size()));
    if (!queued) {
        ++interface.dropped_packets;
        return;
    }
    queued->next = nullptr;
    queued->interface = &interface;
    queued->size = frame.size();
    __builtin_memcpy(queued->data, frame.data(), frame.size());
    bool dropped = false;
    {
        SpinLockGuard guard(m_queue_lock);
        if (m_queue_length >= largest_queued_frames) {
            dropped = true;
        } else {
            if (m_queue_tail)
                m_queue_tail->next = queued;
            else
                m_queue_head = queued;
            m_queue_tail = queued;
            ++m_queue_length;
        }
    }
    if (dropped) {
        ++interface.dropped_packets;
        kfree(queued);
        return;
    }
    wake_thread();
}

void NetworkStack::run()
{
    u64 const tick = Time::ticks_from_milliseconds(TCPSocket::timer_tick_milliseconds);
    for (;;) {
        bool timers = TCPSocket::any_timers_armed();
        if (timers)
            (void)m_wait.wait_until_interruptible([this] { return m_work_pending.load(); }, Time::now() + tick);
        else
            m_wait.wait_until([this] { return m_work_pending.load(); });
        m_work_pending.store(false);

        for (;;) {
            QueuedFrame* frame = nullptr;
            DeferredCall* call = nullptr;
            {
                SpinLockGuard guard(m_queue_lock);
                call = m_deferred;
                if (call)
                    m_deferred = call->next;
                frame = m_queue_head;
                if (frame) {
                    m_queue_head = frame->next;
                    if (!m_queue_head)
                        m_queue_tail = nullptr;
                    --m_queue_length;
                }
            }
            if (!frame && !call)
                break;
            MutexLocker locker(m_lock);
            if (call)
                call->function(call->context);
            if (frame) {
                process_frame(*frame->interface, { frame->data, frame->size });
                kfree(frame);
            }
        }
        MutexLocker locker(m_lock);
        TCPSocket::run_timers();
        expire_neighbors();
    }
}

void NetworkStack::process_frame(NetworkInterface& interface, ReadonlyBytes frame)
{
    ++interface.received_packets;
    interface.received_bytes += frame.size();
    // A way to see what is done about loss, without a network that loses things.
    if (m_drop_rate != 0 && interface.is_loopback() && ++m_drop_counter >= m_drop_rate) {
        m_drop_counter = 0;
        ++interface.dropped_packets;
        return;
    }
    if (interface.is_loopback()) {
        receive_ipv4(interface, frame);
        return;
    }
    if (frame.size() < sizeof(EthernetHeader))
        return;
    auto const& ethernet = *reinterpret_cast<EthernetHeader const*>(frame.data());
    if (!(ethernet.destination == interface.mac()) && !ethernet.destination.is_broadcast())
        return;
    ReadonlyBytes payload { frame.data() + sizeof(EthernetHeader), frame.size() - sizeof(EthernetHeader) };
    switch (static_cast<u16>(ethernet.type)) {
    case ether_type_arp:
        receive_arp(interface, payload);
        break;
    case ether_type_ipv4:
        receive_ipv4(interface, payload);
        break;
    default:
        break;
    }
}

NetworkStack::NeighborEntry* NetworkStack::neighbor_for(IPv4Address address, bool create)
{
    NeighborEntry* oldest = nullptr;
    for (auto* entry : m_neighbors) {
        if (entry->address == address)
            return entry;
        if (!oldest || entry->expires < oldest->expires)
            oldest = entry;
    }
    if (!create)
        return nullptr;
    if (m_neighbors.size() >= largest_neighbors) {
        m_neighbors.remove_all_matching([oldest](auto* entry) { return entry == oldest; });
        delete oldest;
    }
    auto* entry = new (Base::nothrow) NeighborEntry;
    if (!entry)
        return nullptr;
    entry->address = address;
    if (m_neighbors.try_append(entry).is_error()) {
        delete entry;
        return nullptr;
    }
    return entry;
}

void NetworkStack::expire_neighbors()
{
    u64 now = Time::now();
    if (now - m_last_neighbor_check < Time::ticks_from_milliseconds(neighbor_retry_milliseconds))
        return;
    m_last_neighbor_check = now;
    for (size_t i = 0; i < m_neighbors.size();) {
        auto* entry = m_neighbors[i];
        if (entry->resolved) {
            if (now > entry->expires) {
                m_neighbors.remove(i);
                delete entry;
                continue;
            }
        } else if (now - entry->last_request >= Time::ticks_from_milliseconds(neighbor_retry_milliseconds)) {
            if (entry->attempts >= neighbor_attempts) {
                m_neighbors.remove(i);
                delete entry;
                continue;
            }
            ++entry->attempts;
            entry->last_request = now;
            if (entry->interface)
                send_arp(*entry->interface, 1, MACAddress::broadcast(), entry->address);
        }
        ++i;
    }
}

void NetworkStack::send_arp(NetworkInterface& interface, u16 operation, MACAddress target_mac, IPv4Address target_ip)
{
    struct [[gnu::packed]] Frame {
        EthernetHeader ethernet;
        ARPPacket arp;
    } frame;
    frame.ethernet.destination = operation == 1 ? MACAddress::broadcast() : target_mac;
    frame.ethernet.source = interface.mac();
    frame.ethernet.type = ether_type_arp;
    frame.arp.hardware_type = 1;
    frame.arp.protocol_type = ether_type_ipv4;
    frame.arp.hardware_size = 6;
    frame.arp.protocol_size = 4;
    frame.arp.operation = operation;
    frame.arp.sender_mac = interface.mac();
    frame.arp.sender_ip = interface.address.value;
    frame.arp.target_mac = operation == 1 ? MACAddress {} : target_mac;
    frame.arp.target_ip = target_ip.value;
    ++interface.sent_packets;
    interface.sent_bytes += sizeof(frame);
    (void)interface.transmit({ reinterpret_cast<u8 const*>(&frame), sizeof(frame) });
}

void NetworkStack::receive_arp(NetworkInterface& interface, ReadonlyBytes packet)
{
    if (packet.size() < sizeof(ARPPacket))
        return;
    auto const& arp = *reinterpret_cast<ARPPacket const*>(packet.data());
    if (arp.hardware_type != 1 || arp.protocol_type != ether_type_ipv4 || arp.hardware_size != 6 || arp.protocol_size != 4)
        return;
    IPv4Address sender { arp.sender_ip };
    IPv4Address target { arp.target_ip };
    bool for_us = !interface.address.is_zero() && target == interface.address;
    // Learn who sent this, if it is for us or we were already looking for them.
    auto* entry = neighbor_for(sender, for_us);
    if (entry && !sender.is_zero()) {
        entry->mac = arp.sender_mac;
        entry->interface = &interface;
        entry->resolved = true;
        entry->expires = Time::now() + Time::ticks_from_milliseconds(neighbor_lifetime_seconds * 1000);
        auto waiting = move(entry->waiting);
        for (auto const& held : waiting) {
            __builtin_memcpy(m_frame + sizeof(EthernetHeader), held.data, held.size);
            auto& ethernet = *reinterpret_cast<EthernetHeader*>(m_frame);
            ethernet.destination = entry->mac;
            ethernet.source = interface.mac();
            ethernet.type = ether_type_ipv4;
            size_t size = sizeof(EthernetHeader) + held.size;
            ++interface.sent_packets;
            interface.sent_bytes += size;
            (void)interface.transmit({ m_frame, size });
            kfree(held.data);
        }
    }
    if (for_us && arp.operation == 1)
        send_arp(interface, 2, arp.sender_mac, sender);
}

ErrorOr<void> NetworkStack::send_ethernet(NetworkInterface& interface, IPv4Address next_hop, ReadonlyBytes ip_packet)
{
    size_t const size = sizeof(EthernetHeader) + ip_packet.size();
    if (size > sizeof(m_frame))
        return Error::from_errno(EMSGSIZE);

    MACAddress destination;
    if (next_hop.is_limited_broadcast() || next_hop == interface.subnet_broadcast()) {
        destination = MACAddress::broadcast();
    } else {
        auto* entry = neighbor_for(next_hop, true);
        if (!entry)
            return Error::from_errno(ENOMEM);
        if (!entry->resolved) {
            // Held until it is known.
            if (entry->waiting.size() < largest_packets_waiting_per_neighbor) {
                auto* copy = static_cast<u8*>(kmalloc(ip_packet.size()));
                if (copy) {
                    __builtin_memcpy(copy, ip_packet.data(), ip_packet.size());
                    if (entry->waiting.try_append({ copy, ip_packet.size() }).is_error())
                        kfree(copy);
                }
            }
            if (!entry->interface || entry->attempts == 0) {
                entry->interface = &interface;
                entry->attempts = 1;
                entry->last_request = Time::now();
                entry->expires = entry->last_request;
                send_arp(interface, 1, MACAddress::broadcast(), next_hop);
                request_timer_tick();
            }
            return {};
        }
        destination = entry->mac;
    }
    auto& ethernet = *reinterpret_cast<EthernetHeader*>(m_frame);
    ethernet.destination = destination;
    ethernet.source = interface.mac();
    ethernet.type = ether_type_ipv4;
    __builtin_memcpy(m_frame + sizeof(EthernetHeader), ip_packet.data(), ip_packet.size());
    ++interface.sent_packets;
    interface.sent_bytes += size;
    return interface.transmit({ m_frame, size });
}

bool NetworkStack::is_local_address(IPv4Address address)
{
    if (address.is_loopback())
        return true;
    for (auto& interface : m_interfaces) {
        if (!interface->address.is_zero() && interface->address == address)
            return true;
    }
    return false;
}

IPv4Address NetworkStack::source_address_for(IPv4Address destination, u32 bound_interface)
{
    if (destination.is_loopback())
        return { 0x7f000001 };
    NetworkInterface* with_gateway = nullptr;
    for (auto& interface : m_interfaces) {
        if (interface->is_loopback() || (bound_interface != 0 && interface->index() != bound_interface))
            continue;
        if (interface->address == destination || interface->is_on_same_subnet(destination))
            return interface->address;
        if (!with_gateway && !interface->gateway.is_zero())
            with_gateway = interface.ptr();
    }
    if (with_gateway)
        return with_gateway->address;
    // Broadcast, from an interface with no address yet: as DHCP does.
    if (destination.is_limited_broadcast()) {
        for (auto& interface : m_interfaces) {
            if (!interface->is_loopback() && (bound_interface == 0 || interface->index() == bound_interface))
                return interface->address;
        }
    }
    return {};
}

u16 NetworkStack::path_mtu(IPv4Address destination)
{
    (void)destination;
    for (auto& interface : m_interfaces) {
        if (!interface->is_loopback() && !interface->address.is_zero())
            return interface->mtu();
    }
    return 1500;
}

ErrorOr<void> NetworkStack::send_ipv4(IPv4Address source, IPv4Address destination, u8 protocol, u8 time_to_live, ReadonlyBytes header, ReadonlyBytes payload, u32 bound_interface, bool dont_fragment)
{
    size_t const total = sizeof(IPv4Header) + header.size() + payload.size();
    if (total > sizeof(m_packet))
        return Error::from_errno(EMSGSIZE);

    auto& ip = *reinterpret_cast<IPv4Header*>(m_packet);
    ip.version_and_length = 0x45;
    ip.type_of_service = 0;
    ip.total_length = static_cast<u16>(total);
    ip.identification = m_next_identification++;
    ip.flags_and_fragment = dont_fragment ? ip_flag_dont_fragment : 0;
    ip.time_to_live = time_to_live != 0 ? time_to_live : default_time_to_live;
    ip.protocol = protocol;
    ip.checksum = 0;
    ip.source = source.value;
    ip.destination = destination.value;
    Checksum checksum;
    checksum.add({ m_packet, sizeof(IPv4Header) });
    ip.checksum = checksum.result();
    if (!header.is_empty())
        __builtin_memcpy(m_packet + sizeof(IPv4Header), header.data(), header.size());
    if (!payload.is_empty())
        __builtin_memcpy(m_packet + sizeof(IPv4Header) + header.size(), payload.data(), payload.size());
    ReadonlyBytes packet { m_packet, total };

    if (destination.is_loopback() || is_local_address(destination)) {
        auto* loopback = interface_named("lo0"_sv);
        VERIFY(loopback);
        ++loopback->sent_packets;
        loopback->sent_bytes += total;
        return loopback->transmit(packet);
    }

    NetworkInterface* with_gateway = nullptr;
    NetworkInterface* first = nullptr;
    for (auto& interface : m_interfaces) {
        if (interface->is_loopback() || (bound_interface != 0 && interface->index() != bound_interface))
            continue;
        if (!first)
            first = interface.ptr();
        if (destination.is_limited_broadcast())
            return send_ethernet(*interface, destination, packet);
        if (interface->is_on_same_subnet(destination))
            return send_ethernet(*interface, destination, packet);
        if (!with_gateway && !interface->gateway.is_zero())
            with_gateway = interface.ptr();
    }
    if (with_gateway)
        return send_ethernet(*with_gateway, with_gateway->gateway, packet);
    return Error::from_errno(ENETUNREACH);
}

void NetworkStack::send_icmp_error(IPv4Header const& original_header, ReadonlyBytes original, u8 type, u8 code)
{
    IPv4Address destination { original_header.source };
    IPv4Address source { original_header.destination };
    if (destination.is_zero() || destination.is_limited_broadcast() || destination.is_multicast() || source.is_limited_broadcast() || source.is_multicast())
        return;
    // The header that caused it and eight bytes more.
    size_t quoted = min<size_t>(original.size(), sizeof(IPv4Header) + 8);
    ICMPHeader header {};
    header.type = type;
    header.code = code;
    Checksum checksum;
    checksum.add({ reinterpret_cast<u8 const*>(&header), sizeof(header) });
    checksum.add({ original.data(), quoted });
    header.checksum = checksum.result();
    u8 copy[sizeof(header)];
    __builtin_memcpy(copy, &header, sizeof(header));
    // `original` may be in m_packet's way if it came from there: it never does (frames
    // arrive in their own memory).
    (void)send_ipv4(source, destination, protocol_icmp, default_time_to_live, { copy, sizeof(copy) }, { original.data(), quoted });
}

void NetworkStack::receive_icmp(NetworkInterface&, IPv4Header const& ip, ReadonlyBytes packet, ReadonlyBytes payload)
{
    if (payload.size() < sizeof(ICMPHeader))
        return;
    Checksum checksum;
    checksum.add(payload);
    if (checksum.result() != 0)
        return;
    auto const& icmp = *reinterpret_cast<ICMPHeader const*>(payload.data());
    switch (icmp.type) {
    case icmp_echo_request: {
        ICMPHeader reply = icmp;
        reply.type = icmp_echo_reply;
        reply.code = 0;
        reply.checksum = 0;
        ReadonlyBytes rest { payload.data() + sizeof(ICMPHeader), payload.size() - sizeof(ICMPHeader) };
        Checksum reply_checksum;
        reply_checksum.add({ reinterpret_cast<u8 const*>(&reply), sizeof(reply) });
        reply_checksum.add(rest);
        reply.checksum = reply_checksum.result();
        u8 header[sizeof(reply)];
        __builtin_memcpy(header, &reply, sizeof(reply));
        (void)send_ipv4(IPv4Address { ip.destination }, IPv4Address { ip.source }, protocol_icmp, default_time_to_live, { header, sizeof(header) }, rest);
        break;
    }
    case icmp_echo_reply:
        ICMPSocket::deliver(IPv4Address { ip.source }, icmp.identifier, packet);
        break;
    case icmp_destination_unreachable: {
        // What it quotes says which of our sockets to tell.
        ReadonlyBytes quoted { payload.data() + sizeof(ICMPHeader), payload.size() - sizeof(ICMPHeader) };
        if (quoted.size() < sizeof(IPv4Header) + 8)
            return;
        auto const& inner = *reinterpret_cast<IPv4Header const*>(quoted.data());
        size_t inner_length = (inner.version_and_length & 0xf) * 4u;
        if (inner_length < sizeof(IPv4Header) || quoted.size() < inner_length + 4)
            return;
        u8 const* ports = quoted.data() + inner_length;
        u16 source_port = static_cast<u16>(ports[0] << 8 | ports[1]);
        u16 destination_port = static_cast<u16>(ports[2] << 8 | ports[3]);
        int error = icmp.code == icmp_code_port_unreachable ? ECONNREFUSED : EHOSTUNREACH;
        if (inner.protocol == protocol_udp)
            UDPSocket::deliver_error(IPv4Address { inner.destination }, destination_port, source_port, error);
        else if (inner.protocol == protocol_tcp)
            TCPSocket::deliver_error(IPv4Address { inner.destination }, destination_port, source_port, error);
        break;
    }
    default:
        break;
    }
}

void NetworkStack::receive_ipv4(NetworkInterface& interface, ReadonlyBytes packet)
{
    if (packet.size() < sizeof(IPv4Header))
        return;
    auto const& ip = *reinterpret_cast<IPv4Header const*>(packet.data());
    size_t const header_length = (ip.version_and_length & 0xf) * 4u;
    if ((ip.version_and_length >> 4) != 4 || header_length < sizeof(IPv4Header) || packet.size() < header_length)
        return;
    size_t const total_length = ip.total_length;
    if (total_length < header_length || total_length > packet.size())
        return;
    Checksum checksum;
    checksum.add({ packet.data(), header_length });
    if (checksum.result() != 0)
        return;
    // Nothing here sends fragments, and what a server's path breaks up is rare enough to
    // leave to the sender's retrying.
    if (static_cast<u16>(ip.flags_and_fragment) & (ip_flag_more_fragments | ip_fragment_offset_mask)) {
        ++interface.dropped_packets;
        return;
    }

    IPv4Address destination { ip.destination };
    bool mine = interface.is_loopback() || destination == interface.address || destination.is_limited_broadcast()
        || (!interface.address.is_zero() && destination == interface.subnet_broadcast());
    // Until it has an address (asking DHCP), an interface takes what comes to it.
    if (!mine && interface.address.is_zero() && ip.protocol == protocol_udp)
        mine = true;
    if (!mine)
        return;

    ReadonlyBytes whole { packet.data(), total_length };
    ReadonlyBytes payload { packet.data() + header_length, total_length - header_length };
    switch (ip.protocol) {
    case protocol_icmp:
        receive_icmp(interface, ip, whole, payload);
        break;
    case protocol_udp:
        if (!UDPSocket::receive(IPv4Address { ip.source }, destination, payload)
            && !destination.is_limited_broadcast() && !(destination == interface.subnet_broadcast())) {
            send_icmp_error(ip, whole, icmp_destination_unreachable, icmp_code_port_unreachable);
        }
        break;
    case protocol_tcp:
        TCPSocket::receive(IPv4Address { ip.source }, destination, payload);
        break;
    default:
        break;
    }
}

}
