/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/socket.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Net/Protocol.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// IPv4 over Ethernet, and loopback. Everything that touches protocol state holds one lock
// (a desktop has few enough connections for that to be fine). Frames that arrive wait in a
// queue for the network thread, which is also what runs the timers; so nothing here runs in
// an interrupt, and a device needs no locks of the stack's to deliver a frame.

namespace Kernel::Net {

class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;

    [[nodiscard]] StringView name() const { return m_name.view(); }
    [[nodiscard]] MACAddress mac() const { return m_mac; }
    [[nodiscard]] u16 mtu() const { return m_mtu; }
    [[nodiscard]] bool is_loopback() const { return m_is_loopback; }
    [[nodiscard]] u32 index() const { return m_index; }
    void set_index(u32 index) { m_index = index; }

    IPv4Address address;
    IPv4Address netmask;
    IPv4Address gateway;

    [[nodiscard]] bool is_on_same_subnet(IPv4Address other) const
    {
        return !address.is_zero() && (other.value & netmask.value) == (address.value & netmask.value);
    }
    [[nodiscard]] IPv4Address subnet_broadcast() const { return { address.value | ~netmask.value }; }

    // A whole frame: with its Ethernet header, except on loopback. May wait for room, but
    // for nothing that holds the stack's lock.
    virtual ErrorOr<void> transmit(ReadonlyBytes frame) = 0;

    u64 received_packets { 0 };
    u64 received_bytes { 0 };
    u64 sent_packets { 0 };
    u64 sent_bytes { 0 };
    u64 dropped_packets { 0 };

protected:
    NetworkInterface(String name, MACAddress mac, u16 mtu, bool is_loopback)
        : m_name(move(name))
        , m_mac(mac)
        , m_mtu(mtu)
        , m_is_loopback(is_loopback)
    {
    }

private:
    String m_name;
    MACAddress m_mac;
    u16 m_mtu;
    bool m_is_loopback;
    u32 m_index { 0 };
};

class NetworkStack {
public:
    [[nodiscard]] static NetworkStack& the();
    // Makes lo0 and starts the network thread.
    static void initialize();

    [[nodiscard]] Mutex& lock() { return m_lock; }

    // For devices, as they are found: en0 and so on, by the name they ask for.
    void add_interface(NonnullOwnPtr<NetworkInterface>);
    // From any thread, or an interrupt's thread: takes a copy.
    void frame_received(NetworkInterface&, ReadonlyBytes frame);

    // ---- These want the lock held.
    [[nodiscard]] Vector<NonnullOwnPtr<NetworkInterface>>& interfaces() { return m_interfaces; }
    [[nodiscard]] NetworkInterface* interface_named(StringView);
    [[nodiscard]] NetworkInterface* interface_with_index(u32);
    // Which address a packet to `destination` comes from. Zero when there is no route;
    // `bound_interface` (from IP_BOUND_IF, or 0) narrows the choice.
    [[nodiscard]] IPv4Address source_address_for(IPv4Address destination, u32 bound_interface = 0);
    [[nodiscard]] bool is_local_address(IPv4Address);
    // Wraps a header and payload in IP, and out it goes, once its neighbor's address is known.
    ErrorOr<void> send_ipv4(IPv4Address source, IPv4Address destination, u8 protocol, u8 time_to_live, ReadonlyBytes header, ReadonlyBytes payload, u32 bound_interface = 0, bool dont_fragment = false);
    // How much payload fits in a packet to `destination` with no fragmenting.
    [[nodiscard]] u16 path_mtu(IPv4Address destination);
    // A number of packets to lose on purpose, one in N, for tests of what happens then.
    void set_drop_rate(u32 one_in) { m_drop_rate = one_in; m_drop_counter = 0; }
    // ---- End of what wants the lock held.

    // Lock free: asks the network thread to look at its timers within this long.
    void request_timer_tick();
    void wake_thread();
    // Runs on the network thread, soon, with the lock held. (For closing sockets, which may
    // be asked for where nothing may sleep.) The caller keeps the call's memory until then.
    struct DeferredCall {
        DeferredCall* next { nullptr };
        void (*function)(void*) { nullptr };
        void* context { nullptr };
    };
    void defer(DeferredCall&);

private:
    NetworkStack();
    struct QueuedFrame;
    struct NeighborEntry;

    [[noreturn]] void run();
    void process_frame(NetworkInterface&, ReadonlyBytes);
    void receive_arp(NetworkInterface&, ReadonlyBytes);
    void receive_ipv4(NetworkInterface&, ReadonlyBytes);
    void receive_icmp(NetworkInterface&, IPv4Header const&, ReadonlyBytes packet, ReadonlyBytes payload);
    void send_icmp_error(IPv4Header const&, ReadonlyBytes original, u8 type, u8 code);
    void send_arp(NetworkInterface&, u16 operation, MACAddress target_mac, IPv4Address target_ip);
    ErrorOr<void> send_ethernet(NetworkInterface&, IPv4Address next_hop, ReadonlyBytes ip_packet);
    [[nodiscard]] NeighborEntry* neighbor_for(IPv4Address, bool create);
    void expire_neighbors();

    Mutex m_lock;
    Vector<NonnullOwnPtr<NetworkInterface>> m_interfaces;
    Vector<NeighborEntry*> m_neighbors;
    u16 m_next_identification { 1 };
    u32 m_drop_rate { 0 };
    u32 m_drop_counter { 0 };
    u64 m_last_neighbor_check { 0 };

    SpinLock m_queue_lock;
    QueuedFrame* m_queue_head { nullptr };
    QueuedFrame* m_queue_tail { nullptr };
    size_t m_queue_length { 0 };
    DeferredCall* m_deferred { nullptr };
    Atomic<bool> m_work_pending { false };
    Atomic<bool> m_timer_tick_wanted { false };
    WaitQueue m_wait;

    u8 m_packet[2048];
    u8 m_frame[2048];
};

void initialize_network_devices();

}
