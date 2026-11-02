/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/IOBuffer.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Net/NetworkStack.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Vector.h>

namespace Kernel::Net {

class Socket : public RefCounted<Socket> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Socket>> create(int domain, int type, int protocol);
    virtual ~Socket() = default;

    [[nodiscard]] int type() const { return m_type; }

    virtual ErrorOr<void> bind(SocketAddress const&) = 0;
    virtual ErrorOr<void> connect(SocketAddress const&, bool nonblocking) = 0;
    virtual ErrorOr<void> listen(int backlog);
    virtual ErrorOr<NonnullRefPtr<Socket>> accept(bool nonblocking, SocketAddress& peer);
    virtual ErrorOr<size_t> send(IOBuffer const&, size_t count, int flags, SocketAddress const* to, bool nonblocking) = 0;
    virtual ErrorOr<size_t> receive(IOBuffer const&, size_t count, int flags, SocketAddress* from, bool nonblocking) = 0;
    virtual ErrorOr<void> shutdown(int how) = 0;
    [[nodiscard]] virtual SocketAddress local_address() const = 0;
    virtual ErrorOr<SocketAddress> remote_address() const = 0;

    ErrorOr<void> set_option(int level, int name, FlatPtr value, size_t size);
    ErrorOr<void> get_option(int level, int name, FlatPtr value, FlatPtr size_address);

    // For poll(), which holds no locks.
    [[nodiscard]] virtual bool can_read() = 0;
    [[nodiscard]] virtual bool can_write() = 0;
    [[nodiscard]] virtual bool is_hung_up() { return false; }

    // The last descriptor is gone. May be called with spinlocks held: the work is done on
    // the network thread, which keeps this alive until then.
    void detach();

    [[nodiscard]] bool wants_no_sigpipe() const { return m_no_sigpipe; }

    // One row of sysctl net.connections, or false if there is nothing to say.
    [[nodiscard]] virtual bool describe(vulture_connection_info&) { return false; }

protected:
    explicit Socket(int type)
        : m_type(type)
    {
    }

    // With the stack's lock held: what detach() asked for.
    virtual void close_detached() = 0;

    // Everyone waiting on this socket looks again, poll() included.
    void changed();
    [[nodiscard]] u64 change_count() const { return m_changes.load(); }
    // With no locks held. Until something changes after `seen`, a signal, or `deadline` (a
    // Time::now() value, or 0 for none), which gives `timeout_error`.
    ErrorOr<void> wait_for_change(u64 seen, u64 deadline, int timeout_error);

    [[nodiscard]] u64 receive_deadline() const;
    [[nodiscard]] u64 send_deadline() const;

    virtual ErrorOr<void> set_tcp_option(int name, int value);
    virtual ErrorOr<int> get_tcp_option(int name);
    virtual int take_error() { return 0; }

    bool m_reuse_address { false };
    bool m_broadcast { false };
    bool m_keep_alive { false };
    u8 m_time_to_live { 0 };
    u32 m_bound_interface { 0 };
    int m_send_buffer_hint { 0 };
    int m_receive_buffer_hint { 0 };

private:
    int m_type;
    bool m_no_sigpipe { false };
    u64 m_receive_timeout_ticks { 0 };
    u64 m_send_timeout_ticks { 0 };
    Atomic<u64> m_changes { 0 };
    WaitQueue m_wait;
    NetworkStack::DeferredCall m_detach_call;
};

struct Datagram {
    SocketAddress from;
    Vector<u8> bytes;
};

class DatagramSocket : public Socket {
public:
    ErrorOr<size_t> receive(IOBuffer const&, size_t count, int flags, SocketAddress* from, bool nonblocking) override;
    [[nodiscard]] bool can_read() override;
    [[nodiscard]] bool can_write() override { return true; }

protected:
    explicit DatagramSocket(int type)
        : Socket(type)
    {
    }

    // With the lock held. Dropped when too much is waiting already.
    void enqueue(SocketAddress from, ReadonlyBytes);

    Vector<Datagram> m_queue;
    size_t m_queued_bytes { 0 };
    int m_pending_error { 0 };
    bool m_read_shut { false };

    int take_error() override
    {
        int error = m_pending_error;
        m_pending_error = 0;
        return error;
    }
};

class UDPSocket final : public DatagramSocket {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<UDPSocket>> create();

    ErrorOr<void> bind(SocketAddress const&) override;
    ErrorOr<void> connect(SocketAddress const&, bool nonblocking) override;
    ErrorOr<size_t> send(IOBuffer const&, size_t count, int flags, SocketAddress const* to, bool nonblocking) override;
    ErrorOr<void> shutdown(int how) override;
    [[nodiscard]] SocketAddress local_address() const override { return m_local; }
    ErrorOr<SocketAddress> remote_address() const override;
    [[nodiscard]] bool describe(vulture_connection_info&) override;

    // With the lock held. False if no socket wanted it.
    static bool receive(IPv4Address source, IPv4Address destination, ReadonlyBytes udp);
    static void deliver_error(IPv4Address remote, u16 remote_port, u16 local_port, int error);
    static void for_each(void (*)(UDPSocket&, void*), void*);

private:
    UDPSocket()
        : DatagramSocket(SOCK_DGRAM)
    {
    }

    void close_detached() override;
    ErrorOr<void> bind_ephemeral();
    void take_table_reference();

    SocketAddress m_local;
    SocketAddress m_remote;
    bool m_connected { false };
    bool m_in_table { false };
};

// Echoes, as an unprivileged ping does them: SOCK_DGRAM with IPPROTO_ICMP. What is sent is an
// ICMP message; what is received has the IP header in front of it, as on macOS.
class ICMPSocket final : public DatagramSocket {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<ICMPSocket>> create();

    ErrorOr<void> bind(SocketAddress const&) override;
    ErrorOr<void> connect(SocketAddress const&, bool nonblocking) override;
    ErrorOr<size_t> send(IOBuffer const&, size_t count, int flags, SocketAddress const* to, bool nonblocking) override;
    ErrorOr<void> shutdown(int how) override;
    [[nodiscard]] SocketAddress local_address() const override { return {}; }
    ErrorOr<SocketAddress> remote_address() const override;

    // With the lock held.
    static void deliver(IPv4Address from, u16 identifier, ReadonlyBytes ip_packet);

private:
    ICMPSocket()
        : DatagramSocket(SOCK_DGRAM)
    {
    }

    void close_detached() override;

    SocketAddress m_remote;
    bool m_connected { false };
    bool m_in_table { false };
    u16 m_identifier { 0 };
};

[[nodiscard]] u16 pick_random_u16();

}
