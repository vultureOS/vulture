/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/socket.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Net/LocalSocket.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Security/Sandbox.h>
#include <Kernel/Tasks/Process.h>
#include <LibBase/New.h>

namespace Kernel {

namespace {

// Connecting and sending touch two sockets, and sockets refer to each other: one lock for them all is what keeps this simple.
Mutex& the_lock()
{
    static Mutex lock;
    return lock;
}

// Every named socket, by the file that's its name. A file with no socket behind it is what's left when it closed.
struct Name {
    Inode* inode;
    LocalSocket* socket;
};

Vector<Name>& names()
{
    static Vector<Name> list;
    return list;
}

}

LocalSocket::LocalSocket(int type)
    : m_type(type)
{
}

LocalSocket::~LocalSocket()
{
    VERIFY(m_pending.is_empty());
    kfree(m_ring);
}

ErrorOr<NonnullRefPtr<LocalSocket>> LocalSocket::create(int type)
{
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return Error::from_errno(EPROTONOSUPPORT);
    auto* socket = new (Base::nothrow) LocalSocket(type);
    if (!socket)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*socket);
}

ErrorOr<void> LocalSocket::allocate_ring()
{
    if (m_ring)
        return {};
    m_ring = static_cast<u8*>(kmalloc(capacity));
    if (!m_ring)
        return Error::from_errno(ENOMEM);
    return {};
}

ErrorOr<void> LocalSocket::create_pair(int type, RefPtr<LocalSocket>& first, RefPtr<LocalSocket>& second)
{
    auto a = TRY(create(type));
    auto b = TRY(create(type));
    if (type == SOCK_STREAM) {
        TRY(a->allocate_ring());
        TRY(b->allocate_ring());
    }
    auto* process = Process::current();
    MutexLocker locker(the_lock());
    a->m_peer = b;
    b->m_peer = a;
    a->m_was_connected = b->m_was_connected = true;
    if (process) {
        a->m_peer_uid = b->m_peer_uid = process->credentials().euid;
        a->m_peer_gid = b->m_peer_gid = process->credentials().egid;
        a->m_has_peer_credentials = b->m_has_peer_credentials = true;
    }
    first = move(a);
    second = move(b);
    return {};
}

void LocalSocket::wake()
{
    m_changes.fetch_add(1);
    m_wait.wake_all();
    notify_pollers();
}

ErrorOr<void> LocalSocket::wait_for_change(u64 seen)
{
    // (With no locks held. A wait's condition can only look at atomics.)
    if (!m_wait.wait_until_interruptible([&] { return m_changes.load() != seen; }))
        return Error::from_errno(EINTR);
    return {};
}

ErrorOr<void> LocalSocket::bind(StringView path, StringView cwd, u32 permissions)
{
    {
        MutexLocker locker(the_lock());
        if (m_inode)
            return Error::from_errno(EINVAL);
    }
    // EADDRINUSE, as a socket's name is: EEXIST is for files.
    auto made = VirtualFileSystem::make_node(path, cwd, InodeType::Socket, permissions);
    if (made.is_error())
        return made.error().is_errno() && made.error().code() == EEXIST ? Error::from_errno(EADDRINUSE) : made.release_error();
    auto resolved = TRY(VirtualFileSystem::resolve(path, cwd, false));
    MutexLocker locker(the_lock());
    m_inode = resolved.inode;
    // Its name is as it was given, even through a link.
    m_path = TRY(String::from_view(path));
    TRY(names().try_append({ m_inode.ptr(), this }));
    return {};
}

ErrorOr<void> LocalSocket::listen(int backlog)
{
    if (m_type != SOCK_STREAM)
        return Error::from_errno(EOPNOTSUPP);
    MutexLocker locker(the_lock());
    if (!m_inode)
        return Error::from_errno(EINVAL);
    if (m_peer)
        return Error::from_errno(EINVAL);
    m_listening = true;
    m_backlog = backlog <= 0 ? 1 : min(backlog, 128);
    // What whoever connects is told, when they ask who it is: the same as for one that connects.
    if (auto* process = Process::current()) {
        m_peer_uid = process->credentials().euid;
        m_peer_gid = process->credentials().egid;
        m_has_peer_credentials = true;
    }
    return {};
}

ErrorOr<NonnullRefPtr<LocalSocket>> LocalSocket::find_listener(StringView path, StringView cwd, bool for_datagram)
{
    // (Not with the lock: this goes through the filesystem.)
    auto resolved = TRY(VirtualFileSystem::resolve(path, cwd));
    auto metadata = resolved.inode->metadata();
    if (metadata.type != InodeType::Socket)
        return Error::from_errno(ENOTSOCK);
    // Connecting to a socket is writing to it.
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(check_access(metadata, W_OK));
    MutexLocker locker(the_lock());
    for (auto const& name : names()) {
        if (name.inode != resolved.inode.ptr())
            continue;
        if (for_datagram != (name.socket->m_type == SOCK_DGRAM))
            return Error::from_errno(EPROTOTYPE);
        return NonnullRefPtr<LocalSocket>(*name.socket);
    }
    return Error::from_errno(ECONNREFUSED);
}

ErrorOr<void> LocalSocket::connect(StringView path, StringView cwd)
{
    auto target = TRY(find_listener(path, cwd, m_type == SOCK_DGRAM));
    auto* process = Process::current();

    if (m_type == SOCK_DGRAM) {
        MutexLocker locker(the_lock());
        m_peer = target;
        m_was_connected = true;
        return {};
    }

    // The other end of it, for accept() to hand out.
    auto accepted = TRY(create(SOCK_STREAM));
    TRY(accepted->allocate_ring());
    TRY(allocate_ring());
    MutexLocker locker(the_lock());
    if (m_peer || m_was_connected)
        return Error::from_errno(EISCONN);
    if (m_listening)
        return Error::from_errno(EOPNOTSUPP);
    if (!target->m_listening || target->m_detached)
        return Error::from_errno(ECONNREFUSED);
    if (target->m_pending.size() >= static_cast<size_t>(target->m_backlog))
        return Error::from_errno(ECONNREFUSED);
    accepted->m_peer = *this;
    m_peer = accepted;
    m_was_connected = true;
    accepted->m_was_connected = true;
    accepted->m_path = TRY(String::from_view(target->m_path.view()));
    if (process) {
        accepted->m_peer_uid = process->credentials().euid;
        accepted->m_peer_gid = process->credentials().egid;
        accepted->m_has_peer_credentials = true;
    }
    // The other way round is who listened.
    m_peer_uid = target->m_peer_uid;
    m_peer_gid = target->m_peer_gid;
    m_has_peer_credentials = target->m_has_peer_credentials;
    TRY(target->m_pending.try_append(move(accepted)));
    target->wake();
    return {};
}

ErrorOr<NonnullRefPtr<LocalSocket>> LocalSocket::accept(bool nonblocking)
{
    for (;;) {
        u64 seen;
        {
            MutexLocker locker(the_lock());
            if (!m_listening)
                return Error::from_errno(EINVAL);
            if (!m_pending.is_empty()) {
                NonnullRefPtr<LocalSocket> accepted = m_pending[0];
                m_pending.remove(0);
                return accepted;
            }
            if (nonblocking)
                return Error::from_errno(EAGAIN);
            seen = m_changes.load();
        }
        TRY(wait_for_change(seen));
    }
}

ErrorOr<size_t> LocalSocket::send_stream_locked(IOBuffer const& buffer, size_t from, size_t count, Rights& rights)
{
    auto& peer = *m_peer;
    size_t const room = capacity - peer.m_ring_used;
    size_t const total = min(count, room);
    if (total == 0)
        return Error::from_errno(EAGAIN);
    if (!rights.is_empty()) {
        Attachment attachment { peer.m_bytes_written, {} };
        attachment.rights = move(rights);
        rights = {};
        TRY(peer.m_attachments.try_append(move(attachment)));
    }
    size_t done = 0;
    while (done < total) {
        size_t const at = (peer.m_ring_start + peer.m_ring_used) % capacity;
        size_t const chunk = min(total - done, capacity - at);
        TRY(buffer.read(from + done, { peer.m_ring + at, chunk }));
        peer.m_ring_used += chunk;
        peer.m_bytes_written += chunk;
        done += chunk;
    }
    peer.wake();
    return total;
}

ErrorOr<size_t> LocalSocket::send(IOBuffer const& buffer, size_t count, bool nonblocking, Rights rights, StringView to, StringView cwd)
{
    if (m_type == SOCK_DGRAM) {
        RefPtr<LocalSocket> target;
        if (!to.is_empty()) {
            target = TRY(find_listener(to, cwd, true));
        }
        if (count > capacity)
            return Error::from_errno(EMSGSIZE);
        Datagram datagram;
        TRY(datagram.bytes.try_resize(count));
        if (count)
            TRY(buffer.read(0, { datagram.bytes.data(), count }));
        datagram.rights = move(rights);
        for (;;) {
            u64 seen;
            {
                MutexLocker locker(the_lock());
                if (m_write_shut)
                    return Error::from_errno(EPIPE);
                if (!target) {
                    if (!m_peer)
                        return Error::from_errno(m_was_connected ? ECONNREFUSED : ENOTCONN);
                    target = m_peer;
                }
                if (target->m_detached)
                    return Error::from_errno(ECONNREFUSED);
                if (target->m_datagrams.size() < max_queued_datagrams) {
                    datagram.from = TRY(String::from_view(m_path.view()));
                    TRY(target->m_datagrams.try_append(move(datagram)));
                    target->wake();
                    return count;
                }
                if (nonblocking)
                    return Error::from_errno(ENOBUFS);
                seen = target->m_changes.load();
            }
            TRY(target->wait_for_change(seen));
        }
    }

    size_t sent = 0;
    for (;;) {
        RefPtr<LocalSocket> peer;
        u64 seen;
        {
            MutexLocker locker(the_lock());
            if (m_write_shut || m_peer_gone)
                return sent ? ErrorOr<size_t>(sent) : Error::from_errno(EPIPE);
            if (!m_peer)
                return Error::from_errno(m_was_connected ? EPIPE : ENOTCONN);
            if (m_peer->m_read_shut)
                return sent ? ErrorOr<size_t>(sent) : Error::from_errno(EPIPE);
            peer = m_peer;
            if (count == 0)
                return 0;
            auto result = send_stream_locked(buffer, sent, count - sent, rights);
            if (!result.is_error()) {
                sent += result.value();
                if (sent == count)
                    return sent;
            } else if (result.error().code() != EAGAIN) {
                return sent ? ErrorOr<size_t>(sent) : result.release_error();
            }
            if (nonblocking)
                return sent ? ErrorOr<size_t>(sent) : Error::from_errno(EAGAIN);
            seen = peer->m_changes.load();
        }
        // Having sent some is an answer, if a signal comes.
        auto waited = peer->wait_for_change(seen);
        if (waited.is_error())
            return sent ? ErrorOr<size_t>(sent) : waited.release_error();
    }
}

ErrorOr<size_t> LocalSocket::receive(IOBuffer const& buffer, size_t count, bool nonblocking, bool peek, Rights& rights, bool& truncated, String* from)
{
    truncated = false;
    for (;;) {
        u64 seen;
        {
            MutexLocker locker(the_lock());
            if (m_type == SOCK_DGRAM) {
                if (!m_datagrams.is_empty()) {
                    auto& datagram = m_datagrams[0];
                    size_t const taken = min(count, datagram.bytes.size());
                    if (taken)
                        TRY(buffer.write(0, { datagram.bytes.data(), taken }));
                    truncated = datagram.bytes.size() > count;
                    if (from)
                        *from = TRY(String::from_view(datagram.from.view()));
                    if (!peek) {
                        rights = move(datagram.rights);
                        m_datagrams.remove(0);
                        wake();
                    }
                    return taken;
                }
                if (m_read_shut)
                    return 0;
                if (nonblocking)
                    return Error::from_errno(EAGAIN);
            } else {
                if (!m_was_connected && !m_listening)
                    return Error::from_errno(ENOTCONN);
                if (m_ring_used > 0) {
                    size_t total = min(count, m_ring_used);
                    // Rights go with the first byte of what was sent with them, and a read stops short of the next lot.
                    if (!m_attachments.is_empty()) {
                        auto const& first = m_attachments[0];
                        if (first.offset == m_bytes_read) {
                            if (!peek) {
                                rights = move(m_attachments[0].rights);
                                m_attachments.remove(0);
                            }
                            if (!m_attachments.is_empty())
                                total = min<u64>(total, m_attachments[0].offset - m_bytes_read);
                        } else {
                            total = min<u64>(total, first.offset - m_bytes_read);
                        }
                    }
                    size_t done = 0;
                    size_t position = m_ring_start;
                    while (done < total) {
                        size_t const chunk = min(total - done, capacity - position);
                        TRY(buffer.write(done, { m_ring + position, chunk }));
                        position = (position + chunk) % capacity;
                        done += chunk;
                    }
                    if (!peek) {
                        m_ring_start = position;
                        m_ring_used -= total;
                        m_bytes_read += total;
                        // Whoever's waiting to write: that's on this socket's queue.
                        wake();
                    }
                    return total;
                }
                if (m_read_shut || m_peer_gone || m_peer_stopped_writing || (m_was_connected && !m_peer))
                    return 0;
                if (nonblocking)
                    return Error::from_errno(EAGAIN);
            }
            seen = m_changes.load();
        }
        TRY(wait_for_change(seen));
    }
}

ErrorOr<void> LocalSocket::shutdown(int how)
{
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return Error::from_errno(EINVAL);
    MutexLocker locker(the_lock());
    if (m_type == SOCK_STREAM && !m_peer && !m_was_connected)
        return Error::from_errno(ENOTCONN);
    if (how == SHUT_RD || how == SHUT_RDWR)
        m_read_shut = true;
    if (how == SHUT_WR || how == SHUT_RDWR) {
        m_write_shut = true;
        if (m_peer) {
            // It sees the end of the file.
            m_peer->m_peer_stopped_writing = true;
            m_peer->wake();
        }
    }
    wake();
    return {};
}

bool LocalSocket::can_read() const
{
    // (Looked at without the lock, as poll holds none: each of these is only ever a hint to look again.)
    if (m_type == SOCK_DGRAM)
        return !m_datagrams.is_empty() || m_read_shut;
    if (m_listening)
        return !m_pending.is_empty();
    return m_ring_used > 0 || m_read_shut || m_peer_gone || m_peer_stopped_writing || (m_was_connected && !m_peer);
}

bool LocalSocket::can_write() const
{
    if (m_write_shut || m_peer_gone)
        return true;
    if (m_type == SOCK_DGRAM)
        return !m_peer || m_peer->m_datagrams.size() < max_queued_datagrams;
    if (!m_peer)
        return m_was_connected;
    return m_peer->m_ring_used < capacity || m_peer->m_read_shut;
}

bool LocalSocket::is_hung_up() const
{
    return m_type == SOCK_STREAM && !m_listening && m_was_connected && (m_peer_gone || !m_peer);
}

size_t LocalSocket::readable_bytes() const
{
    if (m_type == SOCK_DGRAM)
        return m_datagrams.is_empty() ? 0 : m_datagrams[0].bytes.size();
    if (m_listening)
        return m_pending.size();
    return m_ring_used;
}

void LocalSocket::detach()
{
    // Dropped after the lock's let go: what's dropped can be sockets too, whose last descriptors these were.
    Vector<NonnullRefPtr<LocalSocket>> pending;
    Vector<Datagram> datagrams;
    Vector<Attachment> attachments;
    RefPtr<LocalSocket> peer;
    {
        MutexLocker locker(the_lock());
        m_detached = true;
        names().remove_all_matching([this](auto const& name) { return name.socket == this; });
        pending = move(m_pending);
        m_pending = {};
        datagrams = move(m_datagrams);
        m_datagrams = {};
        attachments = move(m_attachments);
        m_attachments = {};
        peer = move(m_peer);
        m_peer = nullptr;
        if (peer && m_type == SOCK_STREAM) {
            peer->m_peer_gone = true;
            // It keeps what's still to be read from this one, and nothing more of it.
            peer->m_peer = nullptr;
            peer->wake();
        }
        // Writers wait on the socket they're writing to.
        wake();
    }
    for (auto& abandoned : pending)
        abandoned->detach();
}

String LocalSocket::local_path() const
{
    MutexLocker locker(the_lock());
    return MUST(String::from_view(m_path.view()));
}

ErrorOr<String> LocalSocket::peer_path() const
{
    MutexLocker locker(the_lock());
    if (!m_peer)
        return Error::from_errno(ENOTCONN);
    return String::from_view(m_peer->m_path.view());
}

ErrorOr<void> LocalSocket::peer_credentials(u32& uid, u32& gid) const
{
    MutexLocker locker(the_lock());
    if (!m_has_peer_credentials)
        return Error::from_errno(ENOTCONN);
    uid = m_peer_uid;
    gid = m_peer_gid;
    return {};
}

}
