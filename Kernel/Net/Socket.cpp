/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/time.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Net/Socket.h>
#include <Kernel/Net/TCP.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Security/Random.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>

namespace Kernel::Net {

namespace {

constexpr size_t largest_queued_bytes = 256 * KiB;
constexpr u16 first_ephemeral_port = 49152;
constexpr u16 last_ephemeral_port = 65535;

Vector<UDPSocket*>& udp_sockets()
{
    static Vector<UDPSocket*> sockets;
    return sockets;
}

Vector<ICMPSocket*>& icmp_sockets()
{
    static Vector<ICMPSocket*> sockets;
    return sockets;
}

}

u16 pick_random_u16()
{
    u16 value;
    get_random_bytes({ reinterpret_cast<u8*>(&value), sizeof(value) });
    return value;
}

ErrorOr<NonnullRefPtr<Socket>> Socket::create(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return Error::from_errno(EAFNOSUPPORT);
    switch (type) {
    case SOCK_STREAM:
        if (protocol != 0 && protocol != IPPROTO_TCP)
            return Error::from_errno(EPROTONOSUPPORT);
        return NonnullRefPtr<Socket>(TRY(TCPSocket::create()));
    case SOCK_DGRAM:
        if (protocol == IPPROTO_ICMP)
            return NonnullRefPtr<Socket>(TRY(ICMPSocket::create()));
        if (protocol != 0 && protocol != IPPROTO_UDP)
            return Error::from_errno(EPROTONOSUPPORT);
        return NonnullRefPtr<Socket>(TRY(UDPSocket::create()));
    default:
        return Error::from_errno(EPROTONOSUPPORT);
    }
}

ErrorOr<void> Socket::listen(int)
{
    return Error::from_errno(EOPNOTSUPP);
}

ErrorOr<NonnullRefPtr<Socket>> Socket::accept(bool, SocketAddress&)
{
    return Error::from_errno(EOPNOTSUPP);
}

void Socket::changed()
{
    m_changes.fetch_add(1);
    m_wait.wake_all();
    notify_pollers();
}

ErrorOr<void> Socket::wait_for_change(u64 seen, u64 deadline, int timeout_error)
{
    if (deadline == 0) {
        if (!m_wait.wait_until_interruptible([&] { return m_changes.load() != seen; }))
            return Error::from_errno(EINTR);
        return {};
    }
    switch (m_wait.wait_until_interruptible([&] { return m_changes.load() != seen; }, deadline)) {
    case WaitQueue::WaitResult::Satisfied:
        return {};
    case WaitQueue::WaitResult::Interrupted:
        return Error::from_errno(EINTR);
    case WaitQueue::WaitResult::TimedOut:
        return Error::from_errno(timeout_error);
    }
    return {};
}

u64 Socket::receive_deadline() const
{
    return m_receive_timeout_ticks ? Time::now() + m_receive_timeout_ticks : 0;
}

u64 Socket::send_deadline() const
{
    return m_send_timeout_ticks ? Time::now() + m_send_timeout_ticks : 0;
}

void Socket::detach()
{
    // A reference for the network thread to drop, taken here and given up there.
    ref();
    m_detach_call.function = [](void* context) {
        auto* socket = static_cast<Socket*>(context);
        socket->close_detached();
        socket->unref();
    };
    m_detach_call.context = this;
    NetworkStack::the().defer(m_detach_call);
}

ErrorOr<void> Socket::set_tcp_option(int, int)
{
    return Error::from_errno(ENOPROTOOPT);
}

ErrorOr<int> Socket::get_tcp_option(int)
{
    return Error::from_errno(ENOPROTOOPT);
}

ErrorOr<void> Socket::set_option(int level, int name, FlatPtr value, size_t size)
{
    MutexLocker locker(NetworkStack::the().lock());
    if (level == SOL_SOCKET) {
        if (name == SO_SNDTIMEO || name == SO_RCVTIMEO) {
            // A struct timeval: seconds and microseconds, each 64 bits here.
            struct {
                i64 seconds;
                i64 microseconds;
            } time;
            if (size < sizeof(time))
                return Error::from_errno(EINVAL);
            TRY(copy_from_user(&time, value, sizeof(time)));
            if (time.seconds < 0 || time.microseconds < 0)
                return Error::from_errno(EDOM);
            u64 ticks = Time::ticks_from_microseconds(static_cast<u64>(time.seconds) * 1'000'000 + static_cast<u64>(time.microseconds));
            if (name == SO_SNDTIMEO)
                m_send_timeout_ticks = ticks;
            else
                m_receive_timeout_ticks = ticks;
            return {};
        }
    }
    if (size < sizeof(int))
        return Error::from_errno(EINVAL);
    int number = TRY(copy_from_user<int>(value));
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_REUSEADDR:
        case SO_REUSEPORT:
            m_reuse_address = number != 0;
            return {};
        case SO_BROADCAST:
            m_broadcast = number != 0;
            return {};
        case SO_KEEPALIVE:
            m_keep_alive = number != 0;
            return {};
        case SO_NOSIGPIPE:
            m_no_sigpipe = number != 0;
            return {};
        case SO_SNDBUF:
            m_send_buffer_hint = number;
            return {};
        case SO_RCVBUF:
            m_receive_buffer_hint = number;
            return {};
        default:
            return Error::from_errno(ENOPROTOOPT);
        }
    }
    if (level == IPPROTO_IP) {
        switch (name) {
        case IP_TTL:
            if (number < 0 || number > 255)
                return Error::from_errno(EINVAL);
            m_time_to_live = static_cast<u8>(number);
            return {};
        case IP_BOUND_IF:
            m_bound_interface = static_cast<u32>(number);
            return {};
        default:
            return Error::from_errno(ENOPROTOOPT);
        }
    }
    if (level == IPPROTO_TCP)
        return set_tcp_option(name, number);
    return Error::from_errno(ENOPROTOOPT);
}

ErrorOr<void> Socket::get_option(int level, int name, FlatPtr value, FlatPtr size_address)
{
    MutexLocker locker(NetworkStack::the().lock());
    int number = 0;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_ERROR:
            number = take_error();
            break;
        case SO_TYPE:
            number = m_type;
            break;
        case SO_REUSEADDR:
        case SO_REUSEPORT:
            number = m_reuse_address;
            break;
        case SO_BROADCAST:
            number = m_broadcast;
            break;
        case SO_KEEPALIVE:
            number = m_keep_alive;
            break;
        case SO_NOSIGPIPE:
            number = m_no_sigpipe;
            break;
        case SO_SNDBUF:
            number = m_send_buffer_hint ? m_send_buffer_hint : static_cast<int>(256 * KiB);
            break;
        case SO_RCVBUF:
            number = m_receive_buffer_hint ? m_receive_buffer_hint : static_cast<int>(256 * KiB);
            break;
        default:
            return Error::from_errno(ENOPROTOOPT);
        }
    } else if (level == IPPROTO_IP) {
        if (name != IP_TTL)
            return Error::from_errno(ENOPROTOOPT);
        number = m_time_to_live ? m_time_to_live : 64;
    } else if (level == IPPROTO_TCP) {
        number = TRY(get_tcp_option(name));
    } else {
        return Error::from_errno(ENOPROTOOPT);
    }
    auto room = TRY(copy_from_user<u32>(size_address));
    if (room < sizeof(int))
        return Error::from_errno(EINVAL);
    TRY(copy_to_user(value, number));
    TRY(copy_to_user(size_address, static_cast<u32>(sizeof(int))));
    return {};
}

// ---- Datagrams

void DatagramSocket::enqueue(SocketAddress from, ReadonlyBytes bytes)
{
    if (m_read_shut || m_queued_bytes + bytes.size() > largest_queued_bytes)
        return;
    Datagram datagram;
    datagram.from = from;
    if (datagram.bytes.try_resize(bytes.size()).is_error())
        return;
    if (!bytes.is_empty())
        __builtin_memcpy(datagram.bytes.data(), bytes.data(), bytes.size());
    if (m_queue.try_append(move(datagram)).is_error())
        return;
    m_queued_bytes += bytes.size();
    changed();
}

bool DatagramSocket::can_read()
{
    MutexLocker locker(NetworkStack::the().lock());
    return !m_queue.is_empty() || m_pending_error != 0 || m_read_shut;
}

ErrorOr<size_t> DatagramSocket::receive(IOBuffer const& buffer, size_t count, int flags, SocketAddress* from, bool nonblocking)
{
    u64 const deadline = receive_deadline();
    for (;;) {
        u64 seen;
        {
            MutexLocker locker(NetworkStack::the().lock());
            seen = change_count();
            if (m_pending_error != 0)
                return Error::from_errno(take_error());
            if (!m_queue.is_empty()) {
                auto& datagram = m_queue.first();
                size_t size = min(count, datagram.bytes.size());
                if (size > 0)
                    TRY(buffer.write(0, { datagram.bytes.data(), size }));
                if (from)
                    *from = datagram.from;
                if (!(flags & MSG_PEEK)) {
                    m_queued_bytes -= datagram.bytes.size();
                    m_queue.remove(0);
                }
                return size;
            }
            if (m_read_shut)
                return static_cast<size_t>(0);
        }
        if (nonblocking || (flags & MSG_DONTWAIT))
            return Error::from_errno(EAGAIN);
        TRY(wait_for_change(seen, deadline, EAGAIN));
    }
}

// ---- UDP

ErrorOr<NonnullRefPtr<UDPSocket>> UDPSocket::create()
{
    auto* socket = new (Base::nothrow) UDPSocket;
    if (!socket)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*socket);
}

void UDPSocket::take_table_reference()
{
    if (m_in_table)
        return;
    MUST(udp_sockets().try_append(this));
    m_in_table = true;
}

void UDPSocket::close_detached()
{
    if (m_in_table) {
        udp_sockets().remove_all_matching([this](auto* socket) { return socket == this; });
        m_in_table = false;
    }
}

ErrorOr<void> UDPSocket::bind(SocketAddress const& address)
{
    MutexLocker locker(NetworkStack::the().lock());
    if (m_local.port != 0)
        return Error::from_errno(EINVAL);
    auto& stack = NetworkStack::the();
    if (!address.address.is_zero() && !stack.is_local_address(address.address) && !address.address.is_limited_broadcast())
        return Error::from_errno(EADDRNOTAVAIL);
    if (address.port == 0) {
        m_local.address = address.address;
        return bind_ephemeral();
    }
    if (address.port < 1024 && !current_credentials()->is_superuser())
        return Error::from_errno(EACCES);
    for (auto* other : udp_sockets()) {
        if (other->m_local.port == address.port && !(m_reuse_address && other->m_reuse_address))
            return Error::from_errno(EADDRINUSE);
    }
    m_local = address;
    take_table_reference();
    return {};
}

ErrorOr<void> UDPSocket::bind_ephemeral()
{
    u16 const range = last_ephemeral_port - first_ephemeral_port + 1;
    u16 start = static_cast<u16>(pick_random_u16() % range);
    for (u32 i = 0; i < range; ++i) {
        u16 port = static_cast<u16>(first_ephemeral_port + (start + i) % range);
        bool taken = false;
        for (auto* other : udp_sockets()) {
            if (other->m_local.port == port) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            m_local.port = port;
            take_table_reference();
            return {};
        }
    }
    return Error::from_errno(EADDRINUSE);
}

ErrorOr<void> UDPSocket::connect(SocketAddress const& address, bool)
{
    MutexLocker locker(NetworkStack::the().lock());
    // AF_UNSPEC, which arrives here as nothing at all, undoes it.
    if (address.address.is_zero() && address.port == 0) {
        m_connected = false;
        m_remote = {};
        return {};
    }
    if (m_local.port == 0)
        TRY(bind_ephemeral());
    m_remote = address;
    m_connected = true;
    if (m_remote.address.is_zero())
        m_remote.address = { 0x7f000001 };
    if (m_local.address.is_zero())
        m_local.address = NetworkStack::the().source_address_for(m_remote.address, m_bound_interface);
    return {};
}

ErrorOr<SocketAddress> UDPSocket::remote_address() const
{
    if (!m_connected)
        return Error::from_errno(ENOTCONN);
    return m_remote;
}

ErrorOr<void> UDPSocket::shutdown(int how)
{
    MutexLocker locker(NetworkStack::the().lock());
    if (!m_connected)
        return Error::from_errno(ENOTCONN);
    if (how == SHUT_RD || how == SHUT_RDWR) {
        m_read_shut = true;
        changed();
    }
    return {};
}

ErrorOr<size_t> UDPSocket::send(IOBuffer const& buffer, size_t count, int, SocketAddress const* to, bool)
{
    auto& stack = NetworkStack::the();
    MutexLocker locker(stack.lock());
    if (m_pending_error != 0)
        return Error::from_errno(take_error());
    SocketAddress destination;
    if (to) {
        destination = *to;
    } else {
        if (!m_connected)
            return Error::from_errno(EDESTADDRREQ);
        destination = m_remote;
    }
    if (destination.port == 0)
        return Error::from_errno(EINVAL);
    if (destination.address.is_zero())
        destination.address = { 0x7f000001 };
    if (destination.address.is_limited_broadcast() && !m_broadcast)
        return Error::from_errno(EACCES);
    if (m_local.port == 0)
        TRY(bind_ephemeral());

    IPv4Address source = m_local.address;
    if (source.is_zero())
        source = stack.source_address_for(destination.address, m_bound_interface);
    // Without an address, only a broadcast can go: that is how one gets an address.
    if (source.is_zero() && !destination.address.is_limited_broadcast())
        return Error::from_errno(ENETUNREACH);

    if (count + sizeof(UDPHeader) + sizeof(IPv4Header) > stack.path_mtu(destination.address))
        return Error::from_errno(EMSGSIZE);
    u8 payload[1500];
    if (count > 0)
        TRY(buffer.read(0, { payload, count }));
    UDPHeader header {};
    header.source_port = m_local.port;
    header.destination_port = destination.port;
    header.length = static_cast<u16>(sizeof(UDPHeader) + count);
    Checksum checksum;
    checksum.add_pseudo_header(source, destination.address, protocol_udp, static_cast<u16>(sizeof(UDPHeader) + count));
    checksum.add({ reinterpret_cast<u8 const*>(&header), sizeof(header) });
    checksum.add({ payload, count });
    u16 sum = checksum.result();
    header.checksum = sum == 0 ? 0xffff : sum;
    u8 header_bytes[sizeof(header)];
    __builtin_memcpy(header_bytes, &header, sizeof(header));
    TRY(stack.send_ipv4(source, destination.address, protocol_udp, m_time_to_live, { header_bytes, sizeof(header_bytes) }, { payload, count }, m_bound_interface));
    return count;
}

bool UDPSocket::receive(IPv4Address source, IPv4Address destination, ReadonlyBytes udp)
{
    if (udp.size() < sizeof(UDPHeader))
        return true;
    auto const& header = *reinterpret_cast<UDPHeader const*>(udp.data());
    size_t const length = header.length;
    if (length < sizeof(UDPHeader) || length > udp.size())
        return true;
    if (header.checksum != 0) {
        Checksum checksum;
        checksum.add_pseudo_header(source, destination, protocol_udp, static_cast<u16>(length));
        checksum.add({ udp.data(), length });
        if (checksum.result() != 0)
            return true;
    }
    ReadonlyBytes payload { udp.data() + sizeof(UDPHeader), length - sizeof(UDPHeader) };
    bool delivered = false;
    for (auto* socket : udp_sockets()) {
        if (socket->m_local.port != header.destination_port)
            continue;
        if (!socket->m_local.address.is_zero() && !(socket->m_local.address == destination) && !destination.is_limited_broadcast())
            continue;
        if (socket->m_connected && (!(socket->m_remote.address == source) || socket->m_remote.port != header.source_port))
            continue;
        socket->enqueue({ source, header.source_port }, payload);
        delivered = true;
    }
    return delivered;
}

void UDPSocket::deliver_error(IPv4Address remote, u16 remote_port, u16 local_port, int error)
{
    for (auto* socket : udp_sockets()) {
        if (socket->m_local.port == local_port && socket->m_connected && socket->m_remote.address == remote && socket->m_remote.port == remote_port) {
            socket->m_pending_error = error;
            socket->changed();
        }
    }
}

void UDPSocket::for_each(void (*callback)(UDPSocket&, void*), void* context)
{
    for (auto* socket : udp_sockets())
        callback(*socket, context);
}

bool UDPSocket::describe(vulture_connection_info& info)
{
    info.protocol = IPPROTO_UDP;
    info.local_address = __builtin_bswap32(m_local.address.value);
    info.remote_address = __builtin_bswap32(m_remote.address.value);
    info.local_port = __builtin_bswap16(m_local.port);
    info.remote_port = __builtin_bswap16(m_remote.port);
    info.receive_queue = static_cast<unsigned>(m_queued_bytes);
    return true;
}

// ---- ICMP echoes

ErrorOr<NonnullRefPtr<ICMPSocket>> ICMPSocket::create()
{
    auto* socket = new (Base::nothrow) ICMPSocket;
    if (!socket)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*socket);
}

void ICMPSocket::close_detached()
{
    if (m_in_table) {
        icmp_sockets().remove_all_matching([this](auto* socket) { return socket == this; });
        m_in_table = false;
    }
}

ErrorOr<void> ICMPSocket::bind(SocketAddress const&)
{
    return {};
}

ErrorOr<void> ICMPSocket::connect(SocketAddress const& address, bool)
{
    MutexLocker locker(NetworkStack::the().lock());
    m_remote = address;
    m_connected = true;
    return {};
}

ErrorOr<SocketAddress> ICMPSocket::remote_address() const
{
    if (!m_connected)
        return Error::from_errno(ENOTCONN);
    return m_remote;
}

ErrorOr<void> ICMPSocket::shutdown(int)
{
    return Error::from_errno(EOPNOTSUPP);
}

ErrorOr<size_t> ICMPSocket::send(IOBuffer const& buffer, size_t count, int, SocketAddress const* to, bool)
{
    auto& stack = NetworkStack::the();
    MutexLocker locker(stack.lock());
    if (!to && !m_connected)
        return Error::from_errno(EDESTADDRREQ);
    SocketAddress destination = to ? *to : m_remote;
    if (count < sizeof(ICMPHeader))
        return Error::from_errno(EINVAL);
    if (count + sizeof(IPv4Header) > stack.path_mtu(destination.address))
        return Error::from_errno(EMSGSIZE);
    u8 message[1500];
    TRY(buffer.read(0, { message, count }));
    auto& header = *reinterpret_cast<ICMPHeader*>(message);
    // Only echoes, which is all anyone without privileges needs of ICMP.
    if (header.type != icmp_echo_request)
        return Error::from_errno(EINVAL);
    m_identifier = header.identifier;
    if (!m_in_table) {
        TRY(icmp_sockets().try_append(this));
        m_in_table = true;
    }
    header.checksum = 0;
    Checksum checksum;
    checksum.add({ message, count });
    header.checksum = checksum.result();
    IPv4Address source = stack.source_address_for(destination.address, m_bound_interface);
    if (source.is_zero())
        return Error::from_errno(ENETUNREACH);
    TRY(stack.send_ipv4(source, destination.address, protocol_icmp, m_time_to_live, { message, count }, {}, m_bound_interface));
    return count;
}

void ICMPSocket::deliver(IPv4Address from, u16 identifier, ReadonlyBytes ip_packet)
{
    for (auto* socket : icmp_sockets()) {
        if (socket->m_identifier == identifier)
            socket->enqueue({ from, 0 }, ip_packet);
    }
}

}
