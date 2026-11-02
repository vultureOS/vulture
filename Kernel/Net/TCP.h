/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Net/Socket.h>

// TCP (RFC 9293), with window scaling, Reno congestion control (RFC 5681), fast retransmit
// and the retransmission timer of RFC 6298. Data that arrives out of order waits for what
// is missing; what is not acknowledged in time is sent again.

namespace Kernel::Net {

// Bytes in a circle.
class ByteRing {
public:
    ByteRing() = default;
    ~ByteRing();

    ErrorOr<void> allocate();
    [[nodiscard]] bool is_allocated() const { return m_data != nullptr; }
    [[nodiscard]] size_t used() const { return m_used; }
    [[nodiscard]] size_t room() const { return m_data ? capacity - m_used : 0; }
    static constexpr size_t capacity = 256 * KiB;

    ErrorOr<size_t> append_from(IOBuffer const&, size_t offset, size_t count);
    void append(ReadonlyBytes);
    void peek(size_t offset, Bytes output) const;
    ErrorOr<void> copy_to(IOBuffer const&, size_t buffer_offset, size_t offset, size_t count) const;
    void consume(size_t);

private:
    u8* m_data { nullptr };
    size_t m_start { 0 };
    size_t m_used { 0 };
};

class TCPSocket final : public Socket {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<TCPSocket>> create();
    ~TCPSocket() override;

    ErrorOr<void> bind(SocketAddress const&) override;
    ErrorOr<void> connect(SocketAddress const&, bool nonblocking) override;
    ErrorOr<void> listen(int backlog) override;
    ErrorOr<NonnullRefPtr<Socket>> accept(bool nonblocking, SocketAddress& peer) override;
    ErrorOr<size_t> send(IOBuffer const&, size_t count, int flags, SocketAddress const* to, bool nonblocking) override;
    ErrorOr<size_t> receive(IOBuffer const&, size_t count, int flags, SocketAddress* from, bool nonblocking) override;
    ErrorOr<void> shutdown(int how) override;
    [[nodiscard]] SocketAddress local_address() const override { return m_local; }
    ErrorOr<SocketAddress> remote_address() const override;
    [[nodiscard]] bool can_read() override;
    [[nodiscard]] bool can_write() override;
    [[nodiscard]] bool is_hung_up() override;
    [[nodiscard]] bool describe(vulture_connection_info&) override;

    static constexpr u64 timer_tick_milliseconds = 10;

    // With the stack's lock held.
    static void receive(IPv4Address source, IPv4Address destination, ReadonlyBytes segment);
    static void deliver_error(IPv4Address remote, u16 remote_port, u16 local_port, int error);
    static void run_timers();
    static void for_each(void (*)(TCPSocket&, void*), void*);
    // Lock free.
    [[nodiscard]] static bool any_timers_armed();

private:
    enum class State : u8 {
        Closed,
        Listen,
        SynSent,
        SynReceived,
        Established,
        FinWait1,
        FinWait2,
        CloseWait,
        Closing,
        LastAck,
        TimeWait,
    };

    TCPSocket()
        : Socket(SOCK_STREAM)
    {
    }

    void close_detached() override;
    ErrorOr<void> set_tcp_option(int name, int value) override;
    ErrorOr<int> get_tcp_option(int name) override;
    int take_error() override;

    ErrorOr<void> bind_ephemeral();
    [[nodiscard]] bool has_port_conflict(u16 port) const;
    void add_to_table();
    void remove_from_table();
    void set_state(State);
    void finish(int error);
    void receive_segment(TCPHeader const&, ReadonlyBytes options, ReadonlyBytes payload, u32 sequence, u32 acknowledgement, u8 flags, u32 window);
    void receive_in_listen(IPv4Address source, IPv4Address destination, TCPHeader const&, ReadonlyBytes options);
    void receive_in_syn_sent(TCPHeader const&, ReadonlyBytes options, ReadonlyBytes payload, u32 sequence, u32 acknowledgement, u8 flags, u32 window);
    void parse_options(ReadonlyBytes);
    void process_acknowledgement(u32 acknowledgement, u32 window, u32 sequence);
    void process_data(ReadonlyBytes payload, u32 sequence, bool fin);
    void receive_fin();
    void drain_out_of_order();
    void output();
    void send_segment(u32 sequence, u8 flags, size_t payload_offset, size_t payload_size);
    void send_ack_now();
    static void send_reset_for(TCPHeader const&, IPv4Address source, IPv4Address destination, size_t payload_size);
    [[nodiscard]] u32 window_to_advertise();
    void arm_retransmit();
    void on_timers(u64 now);
    void retransmit();
    void update_round_trip(u64 sample);
    [[nodiscard]] size_t unsent_bytes() const;
    void start_time_wait();
    [[nodiscard]] u32 bytes_in_flight() const { return m_send_next - m_send_unacknowledged; }

    struct OutOfOrder {
        u32 sequence;
        Vector<u8> bytes;
    };

    SocketAddress m_local;
    SocketAddress m_remote;
    State m_state { State::Closed };
    int m_error { 0 };
    bool m_in_table { false };
    bool m_detached { false };
    bool m_no_delay { false };

    // Sending. The send buffer starts at the oldest byte not acknowledged.
    ByteRing m_send_buffer;
    u32 m_initial_send_sequence { 0 };
    u32 m_send_unacknowledged { 0 };
    u32 m_send_next { 0 };
    // The furthest sent so far: going back to resend moves m_send_next, not this.
    u32 m_send_highest { 0 };
    u32 m_send_window { 0 };
    u32 m_send_window_sequence { 0 };
    u32 m_send_window_acknowledgement { 0 };
    u8 m_send_scale { 0 };
    u16 m_maximum_segment { 536 };
    u32 m_congestion_window { 0 };
    u32 m_slow_start_threshold { 0xffffffff };
    u32 m_duplicate_acks { 0 };
    bool m_fin_wanted { false };
    bool m_fin_sent { false };
    // Where our FIN is, once it has gone out at all.
    bool m_fin_has_sequence { false };
    u32 m_fin_send_sequence { 0 };

    // Round trips: one segment timed at a time.
    bool m_timing { false };
    u32 m_timed_sequence { 0 };
    u64 m_timed_at { 0 };
    u64 m_smoothed_round_trip { 0 };
    u64 m_round_trip_variation { 0 };
    u64 m_retransmit_timeout { 0 };
    u32 m_retries { 0 };

    // Receiving.
    ByteRing m_receive_buffer;
    u32 m_receive_next { 0 };
    u32 m_advertised_edge { 0 };
    u8 m_receive_scale { 0 };
    bool m_scaling_agreed { false };
    bool m_fin_received { false };
    bool m_read_shut { false };
    u32 m_fin_sequence { 0 };
    bool m_fin_out_of_order { false };
    u32 m_segments_since_ack { 0 };
    Vector<OutOfOrder> m_out_of_order;
    size_t m_out_of_order_bytes { 0 };

    // Timers: when, in Time::now() ticks; 0 for not running.
    u64 m_retransmit_deadline { 0 };
    u64 m_delayed_ack_deadline { 0 };
    u64 m_time_wait_deadline { 0 };
    u64 m_orphan_deadline { 0 };

    // Listening.
    TCPSocket* m_listener { nullptr };
    int m_backlog { 0 };
    u32 m_pending_children { 0 };
    Vector<NonnullRefPtr<TCPSocket>> m_accept_queue;
};

}
