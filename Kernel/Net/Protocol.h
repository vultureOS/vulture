/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Assertions.h>
#include <LibBase/Span.h>
#include <LibBase/Types.h>

// What goes on the wire. Headers are packed, so a field can be at any address, and numbers
// in them are big-endian through BE16 and BE32.

namespace Kernel::Net {

class [[gnu::packed]] BE16 {
public:
    constexpr BE16() = default;
    constexpr BE16(u16 value) { *this = value; }
    constexpr BE16& operator=(u16 value)
    {
        m_bytes[0] = static_cast<u8>(value >> 8);
        m_bytes[1] = static_cast<u8>(value);
        return *this;
    }
    constexpr operator u16() const { return static_cast<u16>(m_bytes[0] << 8 | m_bytes[1]); }

private:
    u8 m_bytes[2] {};
};

class [[gnu::packed]] BE32 {
public:
    constexpr BE32() = default;
    constexpr BE32(u32 value) { *this = value; }
    constexpr BE32& operator=(u32 value)
    {
        m_bytes[0] = static_cast<u8>(value >> 24);
        m_bytes[1] = static_cast<u8>(value >> 16);
        m_bytes[2] = static_cast<u8>(value >> 8);
        m_bytes[3] = static_cast<u8>(value);
        return *this;
    }
    constexpr operator u32() const
    {
        return static_cast<u32>(m_bytes[0]) << 24 | static_cast<u32>(m_bytes[1]) << 16 | static_cast<u32>(m_bytes[2]) << 8 | m_bytes[3];
    }

private:
    u8 m_bytes[4] {};
};

struct [[gnu::packed]] MACAddress {
    u8 bytes[6] {};

    [[nodiscard]] bool operator==(MACAddress const& other) const
    {
        for (int i = 0; i < 6; ++i) {
            if (bytes[i] != other.bytes[i])
                return false;
        }
        return true;
    }
    [[nodiscard]] bool is_broadcast() const { return *this == broadcast(); }
    [[nodiscard]] static MACAddress broadcast()
    {
        MACAddress address;
        for (auto& byte : address.bytes)
            byte = 0xff;
        return address;
    }
};

// Host order, everywhere in the kernel but on the wire and in sockaddr_in.
struct IPv4Address {
    u32 value { 0 };

    [[nodiscard]] constexpr bool operator==(IPv4Address const&) const = default;
    [[nodiscard]] constexpr bool is_zero() const { return value == 0; }
    [[nodiscard]] constexpr bool is_loopback() const { return (value >> 24) == 127; }
    [[nodiscard]] constexpr bool is_limited_broadcast() const { return value == 0xffffffff; }
    [[nodiscard]] constexpr bool is_multicast() const { return (value >> 28) == 0xe; }
};

// An address and port, as a socket is bound or connected to them.
struct SocketAddress {
    IPv4Address address;
    u16 port { 0 };
};

constexpr u16 ether_type_ipv4 = 0x0800;
constexpr u16 ether_type_arp = 0x0806;

struct [[gnu::packed]] EthernetHeader {
    MACAddress destination;
    MACAddress source;
    BE16 type;
};
static_assert(sizeof(EthernetHeader) == 14);

struct [[gnu::packed]] ARPPacket {
    BE16 hardware_type;
    BE16 protocol_type;
    u8 hardware_size;
    u8 protocol_size;
    BE16 operation;
    MACAddress sender_mac;
    BE32 sender_ip;
    MACAddress target_mac;
    BE32 target_ip;
};
static_assert(sizeof(ARPPacket) == 28);

constexpr u8 protocol_icmp = 1;
constexpr u8 protocol_tcp = 6;
constexpr u8 protocol_udp = 17;

struct [[gnu::packed]] IPv4Header {
    u8 version_and_length;
    u8 type_of_service;
    BE16 total_length;
    BE16 identification;
    BE16 flags_and_fragment;
    u8 time_to_live;
    u8 protocol;
    BE16 checksum;
    BE32 source;
    BE32 destination;
};
static_assert(sizeof(IPv4Header) == 20);

constexpr u16 ip_flag_dont_fragment = 0x4000;
constexpr u16 ip_flag_more_fragments = 0x2000;
constexpr u16 ip_fragment_offset_mask = 0x1fff;

constexpr u8 icmp_echo_reply = 0;
constexpr u8 icmp_destination_unreachable = 3;
constexpr u8 icmp_echo_request = 8;
constexpr u8 icmp_code_port_unreachable = 3;

struct [[gnu::packed]] ICMPHeader {
    u8 type;
    u8 code;
    BE16 checksum;
    BE16 identifier;
    BE16 sequence;
};
static_assert(sizeof(ICMPHeader) == 8);

struct [[gnu::packed]] UDPHeader {
    BE16 source_port;
    BE16 destination_port;
    BE16 length;
    BE16 checksum;
};
static_assert(sizeof(UDPHeader) == 8);

constexpr u8 tcp_fin = 0x01;
constexpr u8 tcp_syn = 0x02;
constexpr u8 tcp_rst = 0x04;
constexpr u8 tcp_psh = 0x08;
constexpr u8 tcp_ack = 0x10;

struct [[gnu::packed]] TCPHeader {
    BE16 source_port;
    BE16 destination_port;
    BE32 sequence;
    BE32 acknowledgement;
    u8 data_offset; // In words, in the top four bits.
    u8 flags;
    BE16 window;
    BE16 checksum;
    BE16 urgent;
};
static_assert(sizeof(TCPHeader) == 20);

// The Internet checksum (RFC 1071), fed in pieces, of any lengths.
class Checksum {
public:
    void add(ReadonlyBytes bytes)
    {
        size_t i = 0;
        if (m_odd && !bytes.is_empty()) {
            m_sum += bytes[0];
            i = 1;
            m_odd = false;
        }
        for (; i + 1 < bytes.size(); i += 2)
            m_sum += static_cast<u32>(bytes[i]) << 8 | bytes[i + 1];
        if (i < bytes.size()) {
            m_sum += static_cast<u32>(bytes[i]) << 8;
            m_odd = true;
        }
    }

    void add_pseudo_header(IPv4Address source, IPv4Address destination, u8 protocol, u16 length)
    {
        VERIFY(!m_odd);
        m_sum += source.value >> 16;
        m_sum += source.value & 0xffff;
        m_sum += destination.value >> 16;
        m_sum += destination.value & 0xffff;
        m_sum += protocol;
        m_sum += length;
    }

    [[nodiscard]] u16 result() const
    {
        u64 sum = m_sum;
        while (sum >> 16)
            sum = (sum & 0xffff) + (sum >> 16);
        return static_cast<u16>(~sum);
    }

private:
    u64 m_sum { 0 };
    bool m_odd { false };
};

// Sequence numbers wrap around: these compare them as a distance.
[[nodiscard]] constexpr bool sequence_before(u32 a, u32 b) { return static_cast<i32>(a - b) < 0; }
[[nodiscard]] constexpr bool sequence_at_or_before(u32 a, u32 b) { return static_cast<i32>(a - b) <= 0; }
[[nodiscard]] constexpr bool sequence_after(u32 a, u32 b) { return static_cast<i32>(a - b) > 0; }

}
