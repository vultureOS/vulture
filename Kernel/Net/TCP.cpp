/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Net/TCP.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Security/Random.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>

namespace Kernel::Net {

namespace {

constexpr unsigned ring_order = 6; // 2^6 pages: 256 KiB.
static_assert(ByteRing::capacity == page_size << ring_order);

constexpr u16 first_ephemeral_port = 49152;
constexpr u16 last_ephemeral_port = 65535;
// 2^3: with 16 bits of window, up to 512 KiB, more than the buffer.
constexpr u8 our_window_scale = 3;
constexpr u16 default_mss = 536;
constexpr u32 initial_window_segments = 10;
constexpr u32 duplicate_ack_threshold = 3;
constexpr u32 largest_retries = 12;
constexpr u64 delayed_ack_milliseconds = 40;
constexpr u64 smallest_retransmit_milliseconds = 200;
constexpr u64 largest_retransmit_milliseconds = 60'000;
constexpr u64 initial_retransmit_milliseconds = 1000;
constexpr u64 time_wait_milliseconds = 30'000;
// Closed by its owner and waiting on a peer who may never answer.
constexpr u64 orphan_milliseconds = 60'000;
constexpr size_t largest_out_of_order_segments = 128;

Vector<NonnullRefPtr<TCPSocket>>& connections()
{
    static Vector<NonnullRefPtr<TCPSocket>> table;
    return table;
}

Atomic<u32> s_armed_timers { 0 };

u8 s_segment[2048];

}

// ---- The ring

ByteRing::~ByteRing()
{
    if (m_data)
        (void)physical_memory().free_pages(virtual_to_physical(m_data));
}

ErrorOr<void> ByteRing::allocate()
{
    if (m_data)
        return {};
    auto pages = TRY(physical_memory().allocate_pages(ring_order, PageUse::General));
    m_data = physical_to_virtual(pages);
    return {};
}

ErrorOr<size_t> ByteRing::append_from(IOBuffer const& buffer, size_t offset, size_t count)
{
    size_t const size = min(count, room());
    size_t done = 0;
    while (done < size) {
        size_t at = (m_start + m_used + done) % capacity;
        size_t piece = min(size - done, capacity - at);
        TRY(buffer.read(offset + done, { m_data + at, piece }));
        done += piece;
    }
    m_used += size;
    return size;
}

void ByteRing::append(ReadonlyBytes bytes)
{
    size_t const size = min(bytes.size(), room());
    size_t done = 0;
    while (done < size) {
        size_t at = (m_start + m_used + done) % capacity;
        size_t piece = min(size - done, capacity - at);
        __builtin_memcpy(m_data + at, bytes.data() + done, piece);
        done += piece;
    }
    m_used += size;
}

void ByteRing::peek(size_t offset, Bytes output) const
{
    size_t done = 0;
    while (done < output.size()) {
        size_t at = (m_start + offset + done) % capacity;
        size_t piece = min(output.size() - done, capacity - at);
        __builtin_memcpy(output.data() + done, m_data + at, piece);
        done += piece;
    }
}

ErrorOr<void> ByteRing::copy_to(IOBuffer const& buffer, size_t buffer_offset, size_t offset, size_t count) const
{
    size_t done = 0;
    while (done < count) {
        size_t at = (m_start + offset + done) % capacity;
        size_t piece = min(count - done, capacity - at);
        TRY(buffer.write(buffer_offset + done, { m_data + at, piece }));
        done += piece;
    }
    return {};
}

void ByteRing::consume(size_t count)
{
    VERIFY(count <= m_used);
    m_start = (m_start + count) % capacity;
    m_used -= count;
}

// ---- Connections

ErrorOr<NonnullRefPtr<TCPSocket>> TCPSocket::create()
{
    auto* socket = new (Base::nothrow) TCPSocket;
    if (!socket)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*socket);
}

TCPSocket::~TCPSocket()
{
    if (m_retransmit_deadline)
        s_armed_timers.fetch_sub(1);
}

void TCPSocket::add_to_table()
{
    if (m_in_table)
        return;
    MUST(connections().try_append(*this));
    m_in_table = true;
}

void TCPSocket::remove_from_table()
{
    if (!m_in_table)
        return;
    m_in_table = false;
    // This may be the last reference: keep the object until the function is done.
    NonnullRefPtr<TCPSocket> keep(*this);
    connections().remove_all_matching([this](auto const& socket) { return socket.ptr() == this; });
}

void TCPSocket::set_state(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    changed();
}

// The connection is over: tell whoever is waiting. The table lets go.
void TCPSocket::finish(int error)
{
    if (error != 0)
        m_error = error;
    m_state = State::Closed;
    m_retransmit_deadline = 0;
    m_delayed_ack_deadline = 0;
    m_time_wait_deadline = 0;
    m_orphan_deadline = 0;
    if (m_listener) {
        --m_listener->m_pending_children;
        m_listener = nullptr;
    }
    changed();
    remove_from_table();
}

int TCPSocket::take_error()
{
    int error = m_error;
    m_error = 0;
    return error;
}

bool TCPSocket::has_port_conflict(u16 port) const
{
    for (auto const& other : connections()) {
        if (other.ptr() == this || other->m_local.port != port)
            continue;
        // A connection in TIME_WAIT only blocks those who didn't say it's all right.
        if (m_reuse_address && other->m_state == State::TimeWait)
            continue;
        if (m_reuse_address && other->m_state != State::Listen && other->m_remote.port != 0)
            continue;
        return true;
    }
    return false;
}

ErrorOr<void> TCPSocket::bind_ephemeral()
{
    u16 const range = last_ephemeral_port - first_ephemeral_port + 1;
    u16 start = static_cast<u16>(pick_random_u16() % range);
    for (u32 i = 0; i < range; ++i) {
        u16 port = static_cast<u16>(first_ephemeral_port + (start + i) % range);
        if (!has_port_conflict(port)) {
            m_local.port = port;
            return {};
        }
    }
    return Error::from_errno(EADDRINUSE);
}

ErrorOr<void> TCPSocket::bind(SocketAddress const& address)
{
    auto& stack = NetworkStack::the();
    MutexLocker locker(stack.lock());
    if (m_local.port != 0 || m_state != State::Closed)
        return Error::from_errno(EINVAL);
    if (!address.address.is_zero() && !stack.is_local_address(address.address))
        return Error::from_errno(EADDRNOTAVAIL);
    m_local.address = address.address;
    if (address.port == 0)
        return bind_ephemeral();
    if (address.port < 1024 && !current_credentials()->is_superuser())
        return Error::from_errno(EACCES);
    if (has_port_conflict(address.port))
        return Error::from_errno(EADDRINUSE);
    m_local.port = address.port;
    return {};
}

ErrorOr<void> TCPSocket::listen(int backlog)
{
    auto& stack = NetworkStack::the();
    MutexLocker locker(stack.lock());
    if (m_state == State::Listen) {
        m_backlog = backlog;
        return {};
    }
    if (m_state != State::Closed)
        return Error::from_errno(EINVAL);
    if (m_local.port == 0)
        TRY(bind_ephemeral());
    m_backlog = clamp(backlog, 1, SOMAXCONN);
    m_state = State::Listen;
    add_to_table();
    return {};
}

ErrorOr<NonnullRefPtr<Socket>> TCPSocket::accept(bool nonblocking, SocketAddress& peer)
{
    u64 const deadline = receive_deadline();
    for (;;) {
        u64 seen;
        {
            MutexLocker locker(NetworkStack::the().lock());
            seen = change_count();
            if (m_state != State::Listen)
                return Error::from_errno(EINVAL);
            if (!m_accept_queue.is_empty()) {
                auto child = m_accept_queue.first();
                m_accept_queue.remove(0);
                peer = child->m_remote;
                return NonnullRefPtr<Socket>(move(child));
            }
        }
        if (nonblocking)
            return Error::from_errno(EAGAIN);
        TRY(wait_for_change(seen, deadline, EAGAIN));
    }
}

ErrorOr<void> TCPSocket::connect(SocketAddress const& address, bool nonblocking)
{
    auto& stack = NetworkStack::the();
    u64 seen;
    u64 const deadline = send_deadline();
    {
        MutexLocker locker(stack.lock());
        switch (m_state) {
        case State::Closed:
            break;
        case State::SynSent:
            return Error::from_errno(EALREADY);
        case State::Listen:
            return Error::from_errno(EOPNOTSUPP);
        default:
            return Error::from_errno(EISCONN);
        }
        if (address.port == 0)
            return Error::from_errno(EADDRNOTAVAIL);
        m_remote = address;
        if (m_remote.address.is_zero())
            m_remote.address = { 0x7f000001 };
        if (m_local.address.is_zero()) {
            m_local.address = stack.source_address_for(m_remote.address, m_bound_interface);
            if (m_local.address.is_zero())
                return Error::from_errno(ENETUNREACH);
        }
        if (m_local.port == 0)
            TRY(bind_ephemeral());
        TRY(m_send_buffer.allocate());
        TRY(m_receive_buffer.allocate());

        get_random_bytes({ reinterpret_cast<u8*>(&m_initial_send_sequence), sizeof(m_initial_send_sequence) });
        m_send_unacknowledged = m_initial_send_sequence;
        m_send_next = m_initial_send_sequence + 1;
        m_send_highest = m_send_next;
        m_maximum_segment = default_mss;
        m_congestion_window = initial_window_segments * default_mss;
        m_retransmit_timeout = Time::ticks_from_milliseconds(initial_retransmit_milliseconds);
        m_state = State::SynSent;
        add_to_table();
        send_segment(m_initial_send_sequence, tcp_syn, 0, 0);
        arm_retransmit();
        seen = change_count();
    }
    if (nonblocking)
        return Error::from_errno(EINPROGRESS);
    for (;;) {
        TRY(wait_for_change(seen, deadline, ETIMEDOUT));
        MutexLocker locker(stack.lock());
        seen = change_count();
        if (m_state == State::Established || m_state == State::CloseWait)
            return {};
        if (m_state == State::Closed) {
            int error = take_error();
            return Error::from_errno(error ? error : ECONNREFUSED);
        }
    }
}

ErrorOr<SocketAddress> TCPSocket::remote_address() const
{
    if (m_remote.port == 0 || m_state == State::Listen)
        return Error::from_errno(ENOTCONN);
    return m_remote;
}

// ---- Sending

void TCPSocket::send_segment(u32 sequence, u8 flags, size_t payload_offset, size_t payload_size)
{
    size_t options_size = 0;
    u8* header_bytes = s_segment;
    if (flags & tcp_syn) {
        // MSS and window scale. (Only offer scaling in a SYN+ACK if they did.)
        u16 mss = static_cast<u16>(NetworkStack::the().path_mtu(m_remote.address) - sizeof(IPv4Header) - sizeof(TCPHeader));
        u8* options = header_bytes + sizeof(TCPHeader);
        options[0] = 2;
        options[1] = 4;
        options[2] = static_cast<u8>(mss >> 8);
        options[3] = static_cast<u8>(mss);
        options_size = 4;
        if (!(flags & tcp_ack) || m_scaling_agreed) {
            options[4] = 1;
            options[5] = 3;
            options[6] = 3;
            options[7] = our_window_scale;
            options_size = 8;
        }
    }
    size_t const header_size = sizeof(TCPHeader) + options_size;
    auto& header = *reinterpret_cast<TCPHeader*>(header_bytes);
    header.source_port = m_local.port;
    header.destination_port = m_remote.port;
    header.sequence = sequence;
    header.acknowledgement = (flags & tcp_ack) ? m_receive_next : 0;
    header.data_offset = static_cast<u8>((header_size / 4) << 4);
    header.flags = flags;
    u32 window = window_to_advertise();
    header.window = static_cast<u16>((flags & tcp_syn) ? min<u32>(window, 65535) : min<u32>(window >> (m_scaling_agreed ? m_receive_scale : 0), 65535));
    if (flags & tcp_ack)
        m_advertised_edge = m_receive_next + window;
    header.checksum = 0;
    header.urgent = 0;
    if (payload_size > 0)
        m_send_buffer.peek(payload_offset, { header_bytes + header_size, payload_size });

    size_t const total = header_size + payload_size;
    Checksum checksum;
    checksum.add_pseudo_header(m_local.address, m_remote.address, protocol_tcp, static_cast<u16>(total));
    checksum.add({ header_bytes, total });
    header.checksum = checksum.result();
    (void)NetworkStack::the().send_ipv4(m_local.address, m_remote.address, protocol_tcp, m_time_to_live, { header_bytes, total }, {}, m_bound_interface, true);

    // This ACK covers whatever was waiting to be acknowledged.
    if (flags & tcp_ack) {
        m_segments_since_ack = 0;
        m_delayed_ack_deadline = 0;
    }
}

void TCPSocket::send_ack_now()
{
    send_segment(m_send_next, tcp_ack, 0, 0);
}

u32 TCPSocket::window_to_advertise()
{
    if (!m_receive_buffer.is_allocated())
        return 0;
    size_t room = m_receive_buffer.room();
    // Never take back what has already been offered.
    u32 offered = sequence_after(m_advertised_edge, m_receive_next) ? m_advertised_edge - m_receive_next : 0;
    return static_cast<u32>(max<size_t>(room, offered));
}

size_t TCPSocket::unsent_bytes() const
{
    // The buffer holds from the oldest byte not acknowledged. The SYN took a sequence number
    // and no byte (it is unacknowledged only before data can be sent), and the FIN one too.
    u32 in_flight = bytes_in_flight();
    if (m_fin_sent && in_flight > 0)
        --in_flight;
    size_t used = m_send_buffer.used();
    return used > in_flight ? used - in_flight : 0;
}

void TCPSocket::output()
{
    if (m_state != State::Established && m_state != State::CloseWait && m_state != State::FinWait1 && m_state != State::Closing && m_state != State::LastAck)
        return;
    for (;;) {
        u32 const in_flight = bytes_in_flight();
        u32 const window = min(m_congestion_window, m_send_window);
        size_t const unsent = unsent_bytes();
        u32 const usable = window > in_flight ? window - in_flight : 0;
        size_t length = min<size_t>(min<size_t>(unsent, usable), m_maximum_segment);
        bool const fin = m_fin_wanted && !m_fin_sent && length == unsent;

        if (length == 0 && !fin) {
            // Data waiting and no window at all: the timer becomes a probe of it.
            if (unsent > 0 && !m_retransmit_deadline)
                arm_retransmit();
            return;
        }
        // Nagle's rule: one small segment at a time, unless told not to.
        if (!m_no_delay && !fin && length < m_maximum_segment && in_flight > 0)
            return;

        size_t const offset = m_send_buffer.used() - unsent;
        u8 flags = tcp_ack;
        if (length > 0 && length == unsent)
            flags |= tcp_psh;
        if (fin)
            flags |= tcp_fin;
        send_segment(m_send_next, flags, offset, length);
        if (!m_timing && length > 0) {
            m_timing = true;
            m_timed_sequence = m_send_next;
            m_timed_at = Time::now();
        }
        if (fin) {
            m_fin_sent = true;
            m_fin_has_sequence = true;
            m_fin_send_sequence = m_send_next + static_cast<u32>(length);
        }
        m_send_next += static_cast<u32>(length) + (fin ? 1 : 0);
        if (sequence_after(m_send_next, m_send_highest))
            m_send_highest = m_send_next;
        if (!m_retransmit_deadline)
            arm_retransmit();
        if (fin)
            return;
    }
}

ErrorOr<size_t> TCPSocket::send(IOBuffer const& buffer, size_t count, int flags, SocketAddress const* to, bool nonblocking)
{
    if (to)
        return Error::from_errno(EISCONN);
    auto& stack = NetworkStack::the();
    u64 const deadline = send_deadline();
    size_t done = 0;
    for (;;) {
        u64 seen;
        {
            MutexLocker locker(stack.lock());
            seen = change_count();
            if (m_error != 0) {
                if (done > 0)
                    return done;
                return Error::from_errno(take_error());
            }
            switch (m_state) {
            case State::Established:
            case State::CloseWait:
                break;
            case State::SynSent:
            case State::SynReceived:
                if (nonblocking)
                    return Error::from_errno(EAGAIN);
                goto wait;
            case State::Closed:
            case State::Listen:
                if (m_remote.port == 0)
                    return Error::from_errno(ENOTCONN);
                [[fallthrough]];
            default:
                if (done > 0)
                    return done;
                return Error::from_errno(EPIPE);
            }
            if (m_fin_wanted) {
                if (done > 0)
                    return done;
                return Error::from_errno(EPIPE);
            }
            done += TRY(m_send_buffer.append_from(buffer, done, count - done));
            output();
            if (done == count)
                return done;
        }
    wait:
        if (nonblocking || (flags & MSG_DONTWAIT)) {
            if (done > 0)
                return done;
            return Error::from_errno(EAGAIN);
        }
        auto waited = wait_for_change(seen, deadline, EAGAIN);
        if (waited.is_error()) {
            if (done > 0)
                return done;
            return waited.release_error();
        }
    }
}

// ---- Receiving, for the program

ErrorOr<size_t> TCPSocket::receive(IOBuffer const& buffer, size_t count, int flags, SocketAddress* from, bool nonblocking)
{
    auto& stack = NetworkStack::the();
    u64 const deadline = receive_deadline();
    size_t done = 0;
    for (;;) {
        u64 seen;
        {
            MutexLocker locker(stack.lock());
            seen = change_count();
            size_t available = m_receive_buffer.is_allocated() ? m_receive_buffer.used() : 0;
            if (available > 0) {
                size_t size = min(count - done, available);
                TRY(m_receive_buffer.copy_to(buffer, done, 0, size));
                if (!(flags & MSG_PEEK)) {
                    size_t window_before = window_to_advertise();
                    m_receive_buffer.consume(size);
                    // Tell them of the room once it matters: a segment's worth, or half of it all.
                    if (m_state != State::Closed && m_remote.port != 0) {
                        size_t window_now = m_receive_buffer.room();
                        size_t edge_room = sequence_after(m_advertised_edge, m_receive_next) ? m_advertised_edge - m_receive_next : 0;
                        if (window_now >= edge_room + min<size_t>(ByteRing::capacity / 2, m_maximum_segment * 2) && window_now > window_before / 2)
                            send_ack_now();
                    }
                }
                if (from)
                    *from = m_remote;
                done += size;
                if (done == count || !(flags & MSG_WAITALL) || (flags & MSG_PEEK))
                    return done;
                seen = change_count();
                continue;
            }
            if (done > 0 && !(flags & MSG_WAITALL))
                return done;
            if (m_read_shut || m_fin_received)
                return done;
            if (m_error != 0) {
                if (done > 0)
                    return done;
                return Error::from_errno(take_error());
            }
            if (m_state == State::Closed || m_state == State::Listen) {
                if (m_remote.port == 0 || m_state == State::Listen)
                    return Error::from_errno(ENOTCONN);
                return done;
            }
        }
        if (nonblocking || (flags & MSG_DONTWAIT)) {
            if (done > 0)
                return done;
            return Error::from_errno(EAGAIN);
        }
        auto waited = wait_for_change(seen, deadline, EAGAIN);
        if (waited.is_error()) {
            if (done > 0)
                return done;
            return waited.release_error();
        }
    }
}

ErrorOr<void> TCPSocket::shutdown(int how)
{
    MutexLocker locker(NetworkStack::the().lock());
    if (m_state == State::Closed || m_state == State::Listen || m_state == State::SynSent)
        return Error::from_errno(ENOTCONN);
    if (how == SHUT_RD || how == SHUT_RDWR) {
        m_read_shut = true;
        changed();
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        if (!m_fin_wanted) {
            m_fin_wanted = true;
            if (m_state == State::Established)
                m_state = State::FinWait1;
            else if (m_state == State::CloseWait)
                m_state = State::LastAck;
            output();
            changed();
        }
    }
    return {};
}

void TCPSocket::close_detached()
{
    m_detached = true;
    switch (m_state) {
    case State::Closed:
        remove_from_table();
        return;
    case State::Listen: {
        m_state = State::Closed;
        // Those not yet accepted go with it.
        auto queue = move(m_accept_queue);
        for (auto& child : queue) {
            child->m_listener = nullptr;
            child->send_segment(child->m_send_next, tcp_rst | tcp_ack, 0, 0);
            child->finish(0);
        }
        for (size_t i = 0; i < connections().size();) {
            auto& other = connections()[i];
            if (other->m_listener == this) {
                other->m_listener = nullptr;
                other->send_segment(other->m_send_next, tcp_rst | tcp_ack, 0, 0);
                other->finish(0);
                continue;
            }
            ++i;
        }
        remove_from_table();
        return;
    }
    case State::SynSent:
        finish(0);
        return;
    default:
        break;
    }
    // Data nobody will ever read means the other side should know right away.
    if (m_receive_buffer.is_allocated() && m_receive_buffer.used() > 0 && m_state != State::TimeWait) {
        send_segment(m_send_next, tcp_rst | tcp_ack, 0, 0);
        finish(0);
        return;
    }
    m_read_shut = true;
    if (!m_fin_wanted) {
        m_fin_wanted = true;
        if (m_state == State::Established || m_state == State::SynReceived)
            m_state = State::FinWait1;
        else if (m_state == State::CloseWait)
            m_state = State::LastAck;
        output();
    }
    if (!m_orphan_deadline && m_state != State::TimeWait) {
        m_orphan_deadline = Time::now() + Time::ticks_from_milliseconds(orphan_milliseconds);
        s_armed_timers.fetch_add(0);
    }
    NetworkStack::the().request_timer_tick();
}

// ---- Timers

void TCPSocket::arm_retransmit()
{
    m_retransmit_deadline = Time::now() + m_retransmit_timeout;
    NetworkStack::the().request_timer_tick();
}

void TCPSocket::update_round_trip(u64 sample)
{
    if (m_smoothed_round_trip == 0) {
        m_smoothed_round_trip = sample;
        m_round_trip_variation = sample / 2;
    } else {
        u64 difference = sample > m_smoothed_round_trip ? sample - m_smoothed_round_trip : m_smoothed_round_trip - sample;
        m_round_trip_variation = (3 * m_round_trip_variation + difference) / 4;
        m_smoothed_round_trip = (7 * m_smoothed_round_trip + sample) / 8;
    }
    u64 timeout = m_smoothed_round_trip + 4 * m_round_trip_variation;
    m_retransmit_timeout = clamp(timeout, Time::ticks_from_milliseconds(smallest_retransmit_milliseconds), Time::ticks_from_milliseconds(largest_retransmit_milliseconds));
}

void TCPSocket::retransmit()
{
    if (m_state == State::SynSent) {
        send_segment(m_initial_send_sequence, tcp_syn, 0, 0);
        return;
    }
    if (m_state == State::SynReceived) {
        send_segment(m_initial_send_sequence, tcp_syn | tcp_ack, 0, 0);
        return;
    }
    // Go back: what is outstanding counts as unsent again.
    if (m_fin_sent) {
        m_fin_sent = false;
    }
    m_send_next = m_send_unacknowledged;
    if (m_send_window == 0 && m_send_buffer.used() > 0) {
        // A probe of a closed window: one byte over the edge.
        send_segment(m_send_next, tcp_ack, 0, 1);
        m_send_next += 1;
        if (sequence_after(m_send_next, m_send_highest))
            m_send_highest = m_send_next;
        return;
    }
    output();
}

void TCPSocket::on_timers(u64 now)
{
    NonnullRefPtr<TCPSocket> keep(*this);
    if (m_time_wait_deadline && now >= m_time_wait_deadline) {
        finish(0);
        return;
    }
    if (m_orphan_deadline && now >= m_orphan_deadline) {
        send_segment(m_send_next, tcp_rst | tcp_ack, 0, 0);
        finish(0);
        return;
    }
    if (m_delayed_ack_deadline && now >= m_delayed_ack_deadline)
        send_ack_now();
    if (m_retransmit_deadline && now >= m_retransmit_deadline) {
        if (++m_retries > largest_retries) {
            send_segment(m_send_next, tcp_rst | tcp_ack, 0, 0);
            finish(ETIMEDOUT);
            return;
        }
        u32 flight = bytes_in_flight();
        m_slow_start_threshold = max<u32>(flight / 2, m_maximum_segment * 2u);
        m_congestion_window = m_maximum_segment;
        m_duplicate_acks = 0;
        m_timing = false;
        m_retransmit_timeout = min(m_retransmit_timeout * 2, Time::ticks_from_milliseconds(largest_retransmit_milliseconds));
        m_retransmit_deadline = 0;
        retransmit();
        if (!m_retransmit_deadline && m_state != State::Closed)
            arm_retransmit();
    }
}

void TCPSocket::run_timers()
{
    u64 const now = Time::now();
    u32 armed = 0;
    // A timer may take a connection out of the table, so by index, and going back.
    for (size_t i = connections().size(); i-- > 0;) {
        if (i >= connections().size())
            continue;
        auto* socket = connections()[i].ptr();
        socket->on_timers(now);
    }
    for (auto const& socket : connections()) {
        if (socket->m_retransmit_deadline || socket->m_delayed_ack_deadline || socket->m_time_wait_deadline || socket->m_orphan_deadline)
            ++armed;
    }
    s_armed_timers.store(armed);
}

bool TCPSocket::any_timers_armed()
{
    return s_armed_timers.load() != 0;
}

void TCPSocket::start_time_wait()
{
    m_state = State::TimeWait;
    m_retransmit_deadline = 0;
    m_orphan_deadline = 0;
    m_time_wait_deadline = Time::now() + Time::ticks_from_milliseconds(time_wait_milliseconds);
    NetworkStack::the().request_timer_tick();
    changed();
}

// ---- What arrives

void TCPSocket::send_reset_for(TCPHeader const& incoming, IPv4Address source, IPv4Address destination, size_t payload_size)
{
    if (incoming.flags & tcp_rst)
        return;
    TCPHeader reply {};
    reply.source_port = incoming.destination_port;
    reply.destination_port = incoming.source_port;
    reply.data_offset = 5 << 4;
    if (incoming.flags & tcp_ack) {
        reply.sequence = static_cast<u32>(incoming.acknowledgement);
        reply.flags = tcp_rst;
    } else {
        reply.acknowledgement = static_cast<u32>(incoming.sequence) + static_cast<u32>(payload_size) + ((incoming.flags & tcp_syn) ? 1 : 0) + ((incoming.flags & tcp_fin) ? 1 : 0);
        reply.flags = tcp_rst | tcp_ack;
    }
    Checksum checksum;
    checksum.add_pseudo_header(destination, source, protocol_tcp, sizeof(TCPHeader));
    checksum.add({ reinterpret_cast<u8 const*>(&reply), sizeof(reply) });
    reply.checksum = checksum.result();
    u8 bytes[sizeof(reply)];
    __builtin_memcpy(bytes, &reply, sizeof(reply));
    (void)NetworkStack::the().send_ipv4(destination, source, protocol_tcp, 0, { bytes, sizeof(bytes) }, {}, 0, true);
}

void TCPSocket::receive(IPv4Address source, IPv4Address destination, ReadonlyBytes segment)
{
    if (segment.size() < sizeof(TCPHeader))
        return;
    auto const& header = *reinterpret_cast<TCPHeader const*>(segment.data());
    size_t const header_size = (header.data_offset >> 4) * 4u;
    if (header_size < sizeof(TCPHeader) || header_size > segment.size())
        return;
    Checksum checksum;
    checksum.add_pseudo_header(source, destination, protocol_tcp, static_cast<u16>(segment.size()));
    checksum.add(segment);
    if (checksum.result() != 0)
        return;

    ReadonlyBytes options { segment.data() + sizeof(TCPHeader), header_size - sizeof(TCPHeader) };
    ReadonlyBytes payload { segment.data() + header_size, segment.size() - header_size };
    u16 const destination_port = header.destination_port;
    u16 const source_port = header.source_port;

    // The connection itself first, then whoever listens on the port.
    TCPSocket* found = nullptr;
    TCPSocket* listening = nullptr;
    for (auto const& socket : connections()) {
        if (socket->m_local.port != destination_port)
            continue;
        if (socket->m_state == State::Listen) {
            if (socket->m_local.address.is_zero() || socket->m_local.address == destination)
                listening = socket.ptr();
            continue;
        }
        if (socket->m_remote.port == source_port && socket->m_remote.address == source && socket->m_local.address == destination) {
            found = socket.ptr();
            break;
        }
    }
    if (found) {
        NonnullRefPtr<TCPSocket> keep(*found);
        u32 window = static_cast<u32>(static_cast<u16>(header.window));
        if (!(header.flags & tcp_syn) && found->m_scaling_agreed)
            window <<= found->m_send_scale;
        found->receive_segment(header, options, payload, header.sequence, header.acknowledgement, header.flags, window);
        return;
    }
    if (listening) {
        NonnullRefPtr<TCPSocket> keep(*listening);
        listening->receive_in_listen(source, destination, header, options);
        return;
    }
    send_reset_for(header, source, destination, payload.size());
}

void TCPSocket::parse_options(ReadonlyBytes options)
{
    bool scale_offered = false;
    u8 scale = 0;
    size_t i = 0;
    while (i < options.size()) {
        u8 kind = options[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            ++i;
            continue;
        }
        if (i + 1 >= options.size())
            break;
        u8 length = options[i + 1];
        if (length < 2 || i + length > options.size())
            break;
        if (kind == 2 && length == 4) {
            u16 mss = static_cast<u16>(options[i + 2] << 8 | options[i + 3]);
            if (mss >= 64)
                m_maximum_segment = min(mss, static_cast<u16>(NetworkStack::the().path_mtu(m_remote.address) - sizeof(IPv4Header) - sizeof(TCPHeader)));
        } else if (kind == 3 && length == 3) {
            scale_offered = true;
            scale = min<u8>(options[i + 2], 14);
        }
        i += length;
    }
    m_scaling_agreed = scale_offered;
    m_send_scale = scale_offered ? scale : 0;
    m_receive_scale = scale_offered ? our_window_scale : 0;
}

void TCPSocket::receive_in_listen(IPv4Address source, IPv4Address destination, TCPHeader const& header, ReadonlyBytes options)
{
    if (header.flags & tcp_rst)
        return;
    if (header.flags & tcp_ack) {
        send_reset_for(header, source, destination, 0);
        return;
    }
    if (!(header.flags & tcp_syn))
        return;
    // Too many waiting: as if it was never heard. They will try again.
    if (m_accept_queue.size() + m_pending_children >= static_cast<size_t>(m_backlog))
        return;
    auto created = TCPSocket::create();
    if (created.is_error())
        return;
    auto child = created.release_value();
    if (child->m_send_buffer.allocate().is_error() || child->m_receive_buffer.allocate().is_error())
        return;
    child->m_local = { destination, m_local.port };
    child->m_remote = { source, header.source_port };
    child->m_time_to_live = m_time_to_live;
    child->m_no_delay = m_no_delay;
    child->m_bound_interface = m_bound_interface;
    child->m_listener = this;
    ++m_pending_children;
    child->m_maximum_segment = default_mss;
    child->parse_options(options);
    child->m_receive_next = static_cast<u32>(header.sequence) + 1;
    child->m_advertised_edge = child->m_receive_next;
    get_random_bytes({ reinterpret_cast<u8*>(&child->m_initial_send_sequence), sizeof(child->m_initial_send_sequence) });
    child->m_send_unacknowledged = child->m_initial_send_sequence;
    child->m_send_next = child->m_initial_send_sequence + 1;
    child->m_send_highest = child->m_send_next;
    child->m_send_window = static_cast<u16>(header.window);
    child->m_send_window_sequence = header.sequence;
    child->m_congestion_window = initial_window_segments * child->m_maximum_segment;
    child->m_retransmit_timeout = Time::ticks_from_milliseconds(initial_retransmit_milliseconds);
    child->m_state = State::SynReceived;
    child->add_to_table();
    child->send_segment(child->m_initial_send_sequence, tcp_syn | tcp_ack, 0, 0);
    child->arm_retransmit();
}

void TCPSocket::receive_in_syn_sent(TCPHeader const& header, ReadonlyBytes options, ReadonlyBytes payload, u32 sequence, u32 acknowledgement, u8 flags, u32 window)
{
    bool const acceptable_ack = (flags & tcp_ack) && acknowledgement == m_send_next;
    if ((flags & tcp_ack) && !acceptable_ack) {
        if (!(flags & tcp_rst))
            send_reset_for(header, m_remote.address, m_local.address, payload.size());
        return;
    }
    if (flags & tcp_rst) {
        if (acceptable_ack)
            finish(ECONNREFUSED);
        return;
    }
    if (!(flags & tcp_syn))
        return;
    parse_options(options);
    m_receive_next = sequence + 1;
    m_advertised_edge = m_receive_next;
    m_send_window = static_cast<u16>(window);
    m_send_window_sequence = sequence;
    m_send_window_acknowledgement = acknowledgement;
    m_congestion_window = initial_window_segments * m_maximum_segment;
    if (acceptable_ack) {
        m_send_unacknowledged = acknowledgement;
        m_retransmit_deadline = 0;
        m_retries = 0;
        m_state = State::Established;
        send_ack_now();
        changed();
        output();
    } else {
        // Both opened at once.
        m_state = State::SynReceived;
        send_segment(m_initial_send_sequence, tcp_syn | tcp_ack, 0, 0);
    }
}

void TCPSocket::process_acknowledgement(u32 acknowledgement, u32 window, u32 sequence)
{
    // SYN_RECEIVED becomes a connection when the handshake's last part arrives.
    if (m_state == State::SynReceived) {
        if (sequence_at_or_before(m_send_unacknowledged, acknowledgement) && sequence_at_or_before(acknowledgement, m_send_next)) {
            m_state = State::Established;
            m_send_unacknowledged = m_initial_send_sequence + 1;
            m_send_window = window;
            m_send_window_sequence = sequence;
            m_send_window_acknowledgement = acknowledgement;
            m_retransmit_deadline = 0;
            m_retries = 0;
            if (m_listener) {
                --m_listener->m_pending_children;
                auto* listener = m_listener;
                m_listener = nullptr;
                if (listener->m_state == State::Listen && listener->m_accept_queue.try_append(*this).is_error())
                    (void)this;
                listener->changed();
            }
            changed();
        } else {
            return;
        }
    }

    if (sequence_after(acknowledgement, m_send_highest)) {
        // Acknowledging what was never sent.
        send_ack_now();
        return;
    }

    if (sequence_after(acknowledgement, m_send_unacknowledged)) {
        u32 const acknowledged = acknowledgement - m_send_unacknowledged;
        // What it covers of the buffer: everything but a FIN's sequence number.
        size_t bytes = acknowledged;
        bool const covers_fin = m_fin_has_sequence && sequence_at_or_before(m_send_unacknowledged, m_fin_send_sequence) && sequence_after(acknowledgement, m_fin_send_sequence);
        if (covers_fin) {
            --bytes;
            m_fin_sent = true;
        }
        bytes = min(bytes, m_send_buffer.used());
        m_send_buffer.consume(bytes);
        m_send_unacknowledged = acknowledgement;
        // After going back to resend, this can reach past where we had got to again.
        if (sequence_after(acknowledgement, m_send_next))
            m_send_next = acknowledgement;
        m_retries = 0;

        if (m_timing && sequence_after(acknowledgement, m_timed_sequence)) {
            m_timing = false;
            update_round_trip(Time::now() - m_timed_at);
        }
        if (m_duplicate_acks >= duplicate_ack_threshold)
            m_congestion_window = m_slow_start_threshold;
        else if (m_congestion_window < m_slow_start_threshold)
            m_congestion_window += min<u32>(acknowledged, m_maximum_segment);
        else
            m_congestion_window += max<u32>(m_maximum_segment * m_maximum_segment / max<u32>(m_congestion_window, 1), 1);
        m_duplicate_acks = 0;

        if (m_send_unacknowledged == m_send_highest)
            m_retransmit_deadline = 0;
        else
            arm_retransmit();
        changed();
    } else if (acknowledgement == m_send_unacknowledged && bytes_in_flight() > 0 && window == m_send_window) {
        // The same again, with more out there: something in between was lost.
        if (++m_duplicate_acks == duplicate_ack_threshold) {
            u32 flight = bytes_in_flight();
            m_slow_start_threshold = max<u32>(flight / 2, m_maximum_segment * 2u);
            m_congestion_window = m_slow_start_threshold + duplicate_ack_threshold * m_maximum_segment;
            m_timing = false;
            size_t size = min<size_t>(m_send_buffer.used(), m_maximum_segment);
            if (size > 0)
                send_segment(m_send_unacknowledged, tcp_ack, 0, size);
            arm_retransmit();
        } else if (m_duplicate_acks > duplicate_ack_threshold) {
            m_congestion_window += m_maximum_segment;
        }
    }

    // The window, if this is not older news than what we have.
    if (sequence_after(sequence, m_send_window_sequence)
        || (sequence == m_send_window_sequence && sequence_at_or_before(m_send_window_acknowledgement, acknowledgement))) {
        m_send_window = window;
        m_send_window_sequence = sequence;
        m_send_window_acknowledgement = acknowledgement;
    }

    bool const everything_acknowledged = m_fin_has_sequence && m_send_unacknowledged == m_fin_send_sequence + 1;
    switch (m_state) {
    case State::FinWait1:
        if (everything_acknowledged)
            m_state = State::FinWait2;
        break;
    case State::Closing:
        if (everything_acknowledged)
            start_time_wait();
        break;
    case State::LastAck:
        if (everything_acknowledged)
            finish(0);
        return;
    default:
        break;
    }
    output();
}

void TCPSocket::receive_fin()
{
    m_fin_received = true;
    m_receive_next += 1;
    switch (m_state) {
    case State::SynReceived:
    case State::Established:
        m_state = State::CloseWait;
        break;
    case State::FinWait1:
        if (m_fin_has_sequence && m_send_unacknowledged == m_fin_send_sequence + 1)
            start_time_wait();
        else
            m_state = State::Closing;
        break;
    case State::FinWait2:
        start_time_wait();
        break;
    default:
        break;
    }
    send_ack_now();
    changed();
}

void TCPSocket::drain_out_of_order()
{
    bool moved = true;
    while (moved) {
        moved = false;
        for (size_t i = 0; i < m_out_of_order.size(); ++i) {
            auto& piece = m_out_of_order[i];
            u32 const end = piece.sequence + static_cast<u32>(piece.bytes.size());
            if (sequence_at_or_before(end, m_receive_next)) {
                m_out_of_order_bytes -= piece.bytes.size();
                m_out_of_order.remove(i);
                moved = true;
                break;
            }
            if (sequence_at_or_before(piece.sequence, m_receive_next)) {
                size_t skip = m_receive_next - piece.sequence;
                ReadonlyBytes rest { piece.bytes.data() + skip, piece.bytes.size() - skip };
                size_t take = min(rest.size(), m_receive_buffer.room());
                m_receive_buffer.append({ rest.data(), take });
                m_receive_next += static_cast<u32>(take);
                m_out_of_order_bytes -= piece.bytes.size();
                m_out_of_order.remove(i);
                moved = true;
                break;
            }
        }
    }
    if (m_fin_out_of_order && m_fin_sequence == m_receive_next) {
        m_fin_out_of_order = false;
        receive_fin();
    }
}

void TCPSocket::process_data(ReadonlyBytes payload, u32 sequence, bool fin)
{
    // Nobody is left to read it: they should hear so, not go on sending.
    if (m_detached && !payload.is_empty()) {
        send_segment(m_send_next, tcp_rst | tcp_ack, 0, 0);
        finish(0);
        return;
    }
    bool const can_take_data = m_state == State::Established || m_state == State::FinWait1 || m_state == State::FinWait2;
    if (!can_take_data) {
        // A retransmitted FIN or stray data, after we have all of it.
        if (fin || !payload.is_empty())
            send_ack_now();
        return;
    }
    u32 const fin_sequence = sequence + static_cast<u32>(payload.size());

    if (!payload.is_empty()) {
        if (sequence_before(sequence, m_receive_next)) {
            // Part of it we have already.
            u32 skip = m_receive_next - sequence;
            if (skip >= payload.size()) {
                send_ack_now();
                if (!(fin && fin_sequence == m_receive_next))
                    return;
                payload = {};
            } else {
                payload = { payload.data() + skip, payload.size() - skip };
                sequence = m_receive_next;
            }
        }
        if (!payload.is_empty()) {
            if (sequence == m_receive_next) {
                size_t take = min(payload.size(), m_receive_buffer.room());
                m_receive_buffer.append({ payload.data(), take });
                m_receive_next += static_cast<u32>(take);
                if (take < payload.size())
                    fin = false;
                drain_out_of_order();
                changed();
                ++m_segments_since_ack;
                if (m_segments_since_ack >= 2 || !m_out_of_order.is_empty()) {
                    send_ack_now();
                } else if (!m_delayed_ack_deadline) {
                    m_delayed_ack_deadline = Time::now() + Time::ticks_from_milliseconds(delayed_ack_milliseconds);
                    NetworkStack::the().request_timer_tick();
                }
                // Replies we are about to send carry it anyway; output() does that below.
            } else {
                // A gap: keep this for when it is filled, and say what we are missing.
                bool duplicate = false;
                for (auto const& piece : m_out_of_order) {
                    if (piece.sequence == sequence)
                        duplicate = true;
                }
                if (!duplicate && m_out_of_order.size() < largest_out_of_order_segments && m_out_of_order_bytes + payload.size() <= ByteRing::capacity) {
                    OutOfOrder piece;
                    piece.sequence = sequence;
                    if (!piece.bytes.try_resize(payload.size()).is_error()) {
                        __builtin_memcpy(piece.bytes.data(), payload.data(), payload.size());
                        m_out_of_order_bytes += payload.size();
                        if (m_out_of_order.try_append(move(piece)).is_error())
                            m_out_of_order_bytes -= payload.size();
                    }
                }
                send_ack_now();
            }
        }
    }

    if (fin) {
        if (fin_sequence == m_receive_next) {
            receive_fin();
        } else if (sequence_after(fin_sequence, m_receive_next)) {
            m_fin_out_of_order = true;
            m_fin_sequence = fin_sequence;
        } else {
            send_ack_now();
        }
    }
}

void TCPSocket::receive_segment(TCPHeader const& header, ReadonlyBytes options, ReadonlyBytes payload, u32 sequence, u32 acknowledgement, u8 flags, u32 window)
{
    if (m_state == State::Closed)
        return;
    if (m_state == State::SynSent) {
        receive_in_syn_sent(header, options, payload, sequence, acknowledgement, flags, window);
        return;
    }

    // Is any of it inside what we would take?
    u32 const receive_window = m_receive_buffer.room() == 0 ? 0 : window_to_advertise();
    u32 const length = static_cast<u32>(payload.size()) + ((flags & tcp_syn) ? 1 : 0) + ((flags & tcp_fin) ? 1 : 0);
    bool acceptable;
    if (length == 0)
        acceptable = receive_window == 0 ? sequence == m_receive_next
                                         : (sequence_at_or_before(m_receive_next, sequence) && sequence_before(sequence, m_receive_next + receive_window));
    else if (receive_window == 0)
        acceptable = false;
    else
        acceptable = (sequence_at_or_before(m_receive_next, sequence) && sequence_before(sequence, m_receive_next + receive_window))
            || (sequence_at_or_before(m_receive_next, sequence + length - 1) && sequence_before(sequence + length - 1, m_receive_next + receive_window));
    if (!acceptable) {
        if (!(flags & tcp_rst))
            send_ack_now();
        return;
    }

    if (flags & tcp_rst) {
        // In TIME_WAIT it changes nothing; before the program's connection is accepted, or
        // once it has seen it close, there's nobody to tell.
        if (m_state == State::TimeWait)
            return;
        finish(m_state == State::CloseWait || m_state == State::LastAck || m_state == State::Closing ? EPIPE : ECONNRESET);
        return;
    }
    if (flags & tcp_syn) {
        // A second SYN inside the window, past the handshake, means they have forgotten.
        if (m_state == State::SynReceived && sequence == m_receive_next - 1) {
            send_segment(m_initial_send_sequence, tcp_syn | tcp_ack, 0, 0);
            return;
        }
        send_ack_now();
        return;
    }
    if (!(flags & tcp_ack))
        return;

    process_acknowledgement(acknowledgement, window, sequence);
    if (m_state == State::Closed)
        return;
    process_data(payload, sequence, flags & tcp_fin);
    if (m_state == State::TimeWait && (flags & tcp_fin))
        m_time_wait_deadline = Time::now() + Time::ticks_from_milliseconds(time_wait_milliseconds);
}

void TCPSocket::deliver_error(IPv4Address remote, u16 remote_port, u16 local_port, int error)
{
    for (auto const& socket : connections()) {
        if (socket->m_local.port == local_port && socket->m_remote.port == remote_port && socket->m_remote.address == remote) {
            // A hard error only for an attempt to connect; for the rest, the network may recover.
            if (socket->m_state == State::SynSent) {
                NonnullRefPtr<TCPSocket> keep(*socket);
                keep->finish(error);
            }
            return;
        }
    }
}

void TCPSocket::for_each(void (*callback)(TCPSocket&, void*), void* context)
{
    for (auto const& socket : connections())
        callback(*socket, context);
}

bool TCPSocket::describe(vulture_connection_info& info)
{
    info.protocol = IPPROTO_TCP;
    info.local_address = __builtin_bswap32(m_local.address.value);
    info.remote_address = __builtin_bswap32(m_remote.address.value);
    info.local_port = __builtin_bswap16(m_local.port);
    info.remote_port = __builtin_bswap16(m_remote.port);
    info.state = static_cast<unsigned>(m_state);
    info.receive_queue = m_receive_buffer.is_allocated() ? static_cast<unsigned>(m_receive_buffer.used()) : 0;
    info.send_queue = m_send_buffer.is_allocated() ? static_cast<unsigned>(m_send_buffer.used()) : 0;
    return true;
}

ErrorOr<void> TCPSocket::set_tcp_option(int name, int value)
{
    if (name != TCP_NODELAY)
        return Error::from_errno(ENOPROTOOPT);
    m_no_delay = value != 0;
    output();
    return {};
}

ErrorOr<int> TCPSocket::get_tcp_option(int name)
{
    if (name != TCP_NODELAY)
        return Error::from_errno(ENOPROTOOPT);
    return m_no_delay ? 1 : 0;
}

bool TCPSocket::can_read()
{
    MutexLocker locker(NetworkStack::the().lock());
    if (m_state == State::Listen)
        return !m_accept_queue.is_empty();
    return (m_receive_buffer.is_allocated() && m_receive_buffer.used() > 0) || m_fin_received || m_read_shut || m_error != 0 || (m_state == State::Closed && m_remote.port != 0);
}

bool TCPSocket::can_write()
{
    MutexLocker locker(NetworkStack::the().lock());
    if (m_state == State::Established || m_state == State::CloseWait)
        return m_send_buffer.room() > 0 || m_error != 0;
    // A connection that failed, or isn't going to: it would not block.
    return m_state == State::Closed || m_fin_wanted;
}

bool TCPSocket::is_hung_up()
{
    MutexLocker locker(NetworkStack::the().lock());
    return m_remote.port != 0 && m_state == State::Closed;
}

}
