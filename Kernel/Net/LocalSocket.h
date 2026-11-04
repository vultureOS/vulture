/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/IOBuffer.h>
#include <Kernel/Locking/WaitQueue.h>
#include <LibBase/Atomic.h>
#include <LibBase/Error.h>
#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel {

class FileDescription;
class Inode;

// A Unix-domain socket: between processes on this machine, by a name in the filesystem or as a pair with no name. Stream or
// datagram. Open files can be sent through one (SCM_RIGHTS), which is why this isn't just a pair of pipes.
//
// Everything's under one lock: there aren't many of these, and connecting has to touch two at once.
class LocalSocket final : public RefCounted<LocalSocket> {
public:
    using Rights = Vector<NonnullRefPtr<FileDescription>>;

    [[nodiscard]] static ErrorOr<NonnullRefPtr<LocalSocket>> create(int type);
    // Two sockets already connected to each other.
    static ErrorOr<void> create_pair(int type, RefPtr<LocalSocket>& first, RefPtr<LocalSocket>& second);
    ~LocalSocket();

    [[nodiscard]] int type() const { return m_type; }

    // At this path, which must not exist. `cwd` as for everything in VirtualFileSystem; the file made is a socket's.
    ErrorOr<void> bind(StringView path, StringView cwd, u32 permissions);
    ErrorOr<void> listen(int backlog);
    ErrorOr<NonnullRefPtr<LocalSocket>> accept(bool nonblocking);
    ErrorOr<void> connect(StringView path, StringView cwd);

    // Gathered from `buffer` by the caller. `rights` go with the first byte. To a named socket, for a datagram.
    ErrorOr<size_t> send(IOBuffer const& buffer, size_t count, bool nonblocking, Rights rights, StringView to, StringView cwd);
    // What's there, up to `count`: a stream's stops short of bytes that have rights of their own. `peek` leaves it there.
    ErrorOr<size_t> receive(IOBuffer const& buffer, size_t count, bool nonblocking, bool peek, Rights& rights, bool& truncated, String* from);
    // SHUT_RD, SHUT_WR, SHUT_RDWR.
    ErrorOr<void> shutdown(int how);

    // For poll, and kqueue's data.
    [[nodiscard]] bool can_read() const;
    [[nodiscard]] bool can_write() const;
    [[nodiscard]] bool is_hung_up() const;
    [[nodiscard]] size_t readable_bytes() const;

    // The last descriptor is gone.
    void detach();

    [[nodiscard]] String local_path() const;
    [[nodiscard]] ErrorOr<String> peer_path() const;
    // Who connected: what it was when connect() was called on the other end. ENOTCONN if nothing has.
    ErrorOr<void> peer_credentials(u32& uid, u32& gid) const;

    void set_no_sigpipe(bool value) { m_no_sigpipe.store(value); }
    [[nodiscard]] bool wants_no_sigpipe() const { return m_no_sigpipe.load(); }

    [[nodiscard]] size_t buffer_size() const { return capacity; }

private:
    static constexpr size_t capacity = 64 * 1024;
    // A datagram can't be larger than what could be queued, and a queue has a limit.
    static constexpr size_t max_queued_datagrams = 64;

    explicit LocalSocket(int type);

    struct Datagram {
        Vector<u8> bytes;
        Rights rights;
        String from;
    };
    // Rights that go with the byte `offset`, counted from the first ever put in this socket.
    struct Attachment {
        u64 offset;
        Rights rights;
    };

    [[nodiscard]] static ErrorOr<NonnullRefPtr<LocalSocket>> find_listener(StringView path, StringView cwd, bool for_datagram);
    void wake();
    ErrorOr<void> wait_for_change(u64 seen);
    ErrorOr<size_t> send_stream_locked(IOBuffer const&, size_t from, size_t count, Rights&);
    ErrorOr<void> allocate_ring();

    int m_type;
    Atomic<bool> m_no_sigpipe { false };

    // What this is named, if it is, and the file that's the name.
    String m_path;
    RefPtr<Inode> m_inode;

    bool m_listening { false };
    int m_backlog { 0 };
    Vector<NonnullRefPtr<LocalSocket>> m_pending;

    RefPtr<LocalSocket> m_peer;
    bool m_was_connected { false };
    bool m_peer_gone { false };
    bool m_peer_stopped_writing { false }; // shutdown(), at the other end: there's nothing more to read, but it can still be written to.
    bool m_read_shut { false };
    bool m_write_shut { false };
    bool m_detached { false };
    u32 m_peer_uid { 0 };
    u32 m_peer_gid { 0 };
    bool m_has_peer_credentials { false };

    // A stream's bytes on their way in.
    u8* m_ring { nullptr };
    size_t m_ring_start { 0 };
    size_t m_ring_used { 0 };
    u64 m_bytes_read { 0 };
    u64 m_bytes_written { 0 };
    Vector<Attachment> m_attachments;

    Vector<Datagram> m_datagrams;

    Atomic<u64> m_changes { 0 };
    WaitQueue m_wait;
};

}
