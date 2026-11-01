/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Message.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/port.h>
#include <unistd.h>

namespace IPC {

Port& Port::operator=(Port&& other)
{
    if (this != &other) {
        if (m_fd >= 0)
            close(m_fd);
        m_fd = exchange(other.m_fd, -1);
    }
    return *this;
}

Port::~Port()
{
    if (m_fd >= 0)
        close(m_fd);
}

ErrorOr<Port> Port::duplicate() const
{
    int copy = dup(m_fd);
    if (copy < 0)
        return Error::from_errno(errno);
    fcntl(copy, F_SETFD, FD_CLOEXEC);
    return Port(copy);
}

ErrorOr<Port> Port::make_send() const
{
    int send = port_make_send(m_fd, O_CLOEXEC);
    if (send < 0)
        return Error::from_errno(errno);
    return Port(send);
}

ErrorOr<PortPair> create_port()
{
    int fds[2];
    if (port_create(fds, O_CLOEXEC) < 0)
        return Error::from_errno(errno);
    return PortPair { Port(fds[0]), Port(fds[1]) };
}

Region& Region::operator=(Region&& other)
{
    if (this != &other) {
        if (m_address)
            munmap(m_address, m_size);
        m_address = exchange(other.m_address, nullptr);
        m_size = exchange(other.m_size, 0);
    }
    return *this;
}

Region::~Region()
{
    if (m_address)
        munmap(m_address, m_size);
}

ErrorOr<Region> Region::create(size_t size)
{
    size_t rounded = (size + 4095) & ~static_cast<size_t>(4095);
    void* address = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED)
        return Error::from_errno(errno);
    return Region(address, rounded);
}

Encoder::Encoder(u32 magic, u32 id)
    : m_id(id)
{
    encode(magic);
}

void Encoder::append(void const* data, size_t size)
{
    auto const* bytes = static_cast<u8 const*>(data);
    for (size_t i = 0; i < size; ++i)
        m_bytes.append(bytes[i]);
}

void Encoder::encode(StringView text)
{
    encode(static_cast<u32>(text.length()));
    append(text.characters_without_null_termination(), text.length());
}

void Encoder::encode(ReadonlyBytes bytes)
{
    encode(static_cast<u32>(bytes.size()));
    if (bytes.size() > out_of_line_threshold) {
        // Out of line. (Without memory for it, the bytes stay inline, and
        // a message too big for that fails to send.)
        auto region = Region::create(bytes.size());
        if (!region.is_error() && m_regions.size() < VULTURE_PORT_MAX_REGIONS) {
            memcpy(region.value().data(), bytes.data(), bytes.size());
            encode(static_cast<u32>(m_regions.size()));
            m_regions.append(region.release_value());
            return;
        }
        encode(static_cast<u32>(0xffffffff));
    }
    append(bytes.data(), bytes.size());
}

void Encoder::encode(Port&& port)
{
    encode(static_cast<u32>(port.is_valid() ? m_rights.size() : 0xffffffff));
    if (port.is_valid())
        m_rights.append(move(port));
}

ErrorOr<void> Decoder::expect_magic(u32 magic)
{
    u32 found = 0;
    TRY(decode(found));
    if (found != magic)
        return Error::from_errno(EBADMSG);
    return {};
}

ErrorOr<void> Decoder::take(void* destination, size_t size)
{
    if (size > m_message.bytes.size() - m_position)
        return Error::from_errno(EBADMSG);
    memcpy(destination, m_message.bytes.data() + m_position, size);
    m_position += size;
    return {};
}

ErrorOr<void> Decoder::decode(bool& value)
{
    u8 byte = 0;
    TRY(decode(byte));
    if (byte > 1)
        return Error::from_errno(EBADMSG);
    value = byte;
    return {};
}

ErrorOr<void> Decoder::decode(String& value)
{
    u32 length = 0;
    TRY(decode(length));
    if (length > m_message.bytes.size() - m_position)
        return Error::from_errno(EBADMSG);
    value = TRY(String::from_view(StringView(reinterpret_cast<char const*>(m_message.bytes.data() + m_position), length)));
    m_position += length;
    return {};
}

ErrorOr<void> Decoder::decode(Vector<u8>& value)
{
    u32 length = 0;
    TRY(decode(length));
    if (length > out_of_line_threshold) {
        u32 index = 0;
        TRY(decode(index));
        if (index != 0xffffffff) {
            if (index >= m_message.regions.size() || m_message.regions[index].size() < length)
                return Error::from_errno(EBADMSG);
            TRY(value.try_resize(length));
            memcpy(value.data(), m_message.regions[index].data(), length);
            return {};
        }
    }
    if (length > m_message.bytes.size() - m_position)
        return Error::from_errno(EBADMSG);
    TRY(value.try_resize(length));
    memcpy(value.data(), m_message.bytes.data() + m_position, length);
    m_position += length;
    return {};
}

ErrorOr<void> Decoder::decode(Port& value)
{
    u32 index = 0;
    TRY(decode(index));
    if (index == 0xffffffff) {
        value = Port();
        return {};
    }
    if (index >= m_message.rights.size() || !m_message.rights[index].is_valid())
        return Error::from_errno(EBADMSG);
    value = move(m_message.rights[index]);
    return {};
}

namespace {

// Room for a message to start with: most are small; a bigger one waits
// (or, for an answer, comes through the port the kernel hands over) while
// the room grows.
constexpr size_t initial_capacity = 512;

// The message for the kernel. It shares the encoder's descriptors and
// memory (copy-on-write) rather than taking them: the encoder closes and
// unmaps its own once the message is gone, sent or not, so nothing here
// has to work out what the kernel took.
struct Outgoing {
    vulture_port_message message {};
    int fds[VULTURE_PORT_MAX_RIGHTS];
    vulture_port_region regions[VULTURE_PORT_MAX_REGIONS];

    explicit Outgoing(Encoder& encoder)
    {
        message.id = encoder.id();
        message.bytes = encoder.bytes().data();
        message.size = encoder.bytes().size();
        message.reply = -1;
        auto& rights = encoder.rights();
        for (size_t i = 0; i < rights.size() && i < VULTURE_PORT_MAX_RIGHTS; ++i)
            fds[i] = rights[i].fd();
        message.rights = fds;
        message.right_count = static_cast<unsigned>(rights.size());
        auto& memory = encoder.regions();
        for (size_t i = 0; i < memory.size() && i < VULTURE_PORT_MAX_REGIONS; ++i)
            regions[i] = { memory[i].data(), memory[i].size() };
        message.regions = regions;
        message.region_count = static_cast<unsigned>(memory.size());
    }
};

// Where the kernel puts a received message's descriptors and memory.
struct Incoming {
    int rights[VULTURE_PORT_MAX_RIGHTS];
    vulture_port_region regions[VULTURE_PORT_MAX_REGIONS];
};

// A received message's parts into `message`.
ErrorOr<void> adopt(ReceivedMessage& message, vulture_port_received const& received, Incoming const& incoming)
{
    TRY(message.bytes.try_resize(received.size));
    message.id = received.id;
    for (unsigned i = 0; i < received.right_count; ++i)
        TRY(message.rights.try_append(Port(incoming.rights[i])));
    for (unsigned i = 0; i < received.region_count; ++i)
        TRY(message.regions.try_append(Region(incoming.regions[i].address, incoming.regions[i].size)));
    message.reply = Port(received.reply);
    message.sender = { received.sender_pid, received.sender_uid, received.sender_gid };
    return {};
}

void prepare(vulture_port_received& received, ReceivedMessage& message, Incoming& incoming)
{
    received = {};
    received.bytes = message.bytes.data();
    received.capacity = message.bytes.size();
    received.rights = incoming.rights;
    received.right_capacity = VULTURE_PORT_MAX_RIGHTS;
    received.regions = incoming.regions;
    received.region_capacity = VULTURE_PORT_MAX_REGIONS;
    received.reply = -1;
}

}

ErrorOr<void> send(Port const& send_right, Encoder& encoder, bool nonblocking)
{
    if (encoder.rights().size() > VULTURE_PORT_MAX_RIGHTS)
        return Error::from_errno(EMSGSIZE);
    Outgoing outgoing(encoder);
    if (port_send(send_right.fd(), &outgoing.message, nonblocking ? VULTURE_PORT_NONBLOCK : 0) < 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<ReceivedMessage> call(Port const& send_right, Encoder& encoder)
{
    if (encoder.rights().size() > VULTURE_PORT_MAX_RIGHTS)
        return Error::from_errno(EMSGSIZE);
    Outgoing outgoing(encoder);
    ReceivedMessage message;
    TRY(message.bytes.try_resize(initial_capacity));
    Incoming incoming;
    vulture_port_received received;
    prepare(received, message, incoming);
    int result = port_call(send_right.fd(), &outgoing.message, &received, 0);
    int error = errno;
    if (result < 0) {
        // Too big: the answer waits on the port we were given.
        if (error == EMSGSIZE && received.reply >= 0) {
            Port answers(received.reply);
            return receive(answers);
        }
        return Error::from_errno(error);
    }
    TRY(adopt(message, received, incoming));
    return message;
}

ErrorOr<ReceivedMessage> receive(Port const& receive_right, bool nonblocking)
{
    ReceivedMessage message;
    TRY(message.bytes.try_resize(initial_capacity));
    Incoming incoming;
    for (;;) {
        vulture_port_received received;
        prepare(received, message, incoming);
        if (port_receive(receive_right.fd(), &received, nonblocking ? VULTURE_PORT_NONBLOCK : 0) < 0) {
            // Too big: the message waits while we make room.
            if (errno == EMSGSIZE && received.size > message.bytes.size()) {
                TRY(message.bytes.try_resize(received.size));
                continue;
            }
            return Error::from_errno(errno);
        }
        TRY(adopt(message, received, incoming));
        return message;
    }
}

ErrorOr<void> answer(ReceivedMessage& request, Encoder& encoder)
{
    if (!request.reply.is_valid())
        return Error::from_errno(EINVAL);
    auto result = send(request.reply, encoder);
    request.reply = Port();
    return result;
}

}
