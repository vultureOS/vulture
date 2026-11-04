/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/IPC/Port.h>
#include <Kernel/FileSystem/FileSystem.h>
#include <Kernel/FileSystem/KQueue.h>
#include <Kernel/FileSystem/Pipe.h>
#include <Kernel/Net/LocalSocket.h>
#include <Kernel/Net/Socket.h>
#include <Kernel/Tasks/Process.h>
#include <LibBase/New.h>

namespace Kernel {

static Atomic<u64> s_next_serial { 1 };

FileDescription::FileDescription(int flags)
    : m_serial(s_next_serial.fetch_add(1))
    , m_flags(flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC | O_NOFOLLOW))
{
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create(NonnullRefPtr<Inode> inode, int flags)
{
    auto* description = new (Base::nothrow) FileDescription(flags);
    if (!description)
        return Error::from_errno(ENOMEM);
    description->m_inode = move(inode);
    description->m_inode->description_opened();
    return adopt_ref(*description);
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create_for_pipe(NonnullRefPtr<Pipe> pipe, bool is_writer)
{
    auto* description = new (Base::nothrow) FileDescription(is_writer ? O_WRONLY : O_RDONLY);
    if (!description)
        return Error::from_errno(ENOMEM);
    pipe->attach(is_writer);
    description->m_pipe = move(pipe);
    description->m_is_pipe_writer = is_writer;
    return adopt_ref(*description);
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create_for_port(NonnullRefPtr<Port> port, bool receive_right)
{
    auto* description = new (Base::nothrow) FileDescription(receive_right ? O_RDONLY : O_WRONLY);
    if (!description)
        return Error::from_errno(ENOMEM);
    port->attach(receive_right);
    description->m_port = move(port);
    description->m_is_port_receiver = receive_right;
    return adopt_ref(*description);
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create_for_socket(NonnullRefPtr<Net::Socket> socket)
{
    auto* description = new (Base::nothrow) FileDescription(O_RDWR);
    if (!description)
        return Error::from_errno(ENOMEM);
    description->m_socket = move(socket);
    return adopt_ref(*description);
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create_for_local_socket(NonnullRefPtr<LocalSocket> socket)
{
    auto* description = new (Base::nothrow) FileDescription(O_RDWR);
    if (!description)
        return Error::from_errno(ENOMEM);
    description->m_local_socket = move(socket);
    return adopt_ref(*description);
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create_for_kqueue(NonnullRefPtr<KQueue> queue)
{
    auto* description = new (Base::nothrow) FileDescription(O_RDONLY);
    if (!description)
        return Error::from_errno(ENOMEM);
    description->m_kqueue = move(queue);
    return adopt_ref(*description);
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create_for_fifo(NonnullRefPtr<Inode> inode, NonnullRefPtr<Pipe> pipe, int flags)
{
    auto* description = new (Base::nothrow) FileDescription(flags);
    if (!description)
        return Error::from_errno(ENOMEM);
    int const access = flags & O_ACCMODE;
    description->m_is_pipe_both = access == O_RDWR;
    description->m_is_pipe_writer = access == O_WRONLY;
    if (access != O_WRONLY)
        pipe->attach(false);
    if (access != O_RDONLY)
        pipe->attach(true);
    description->m_pipe = move(pipe);
    description->m_inode = move(inode);
    return adopt_ref(*description);
}

FileDescription::~FileDescription()
{
    if (m_socket)
        m_socket->detach();
    if (m_local_socket)
        m_local_socket->detach();
    if (m_pipe && m_inode) {
        if (m_is_pipe_both || !m_is_pipe_writer)
            m_pipe->detach(false);
        if (m_is_pipe_both || m_is_pipe_writer)
            m_pipe->detach(true);
        Pipe::forget_fifo_if_unused(*m_inode, *m_pipe);
        return;
    }
    if (m_inode)
        m_inode->description_closed();
    if (m_pipe)
        m_pipe->detach(m_is_pipe_writer);
    if (m_port)
        m_port->detach(m_is_port_receiver);
}

bool FileDescription::can_read() const
{
    if (m_socket)
        return m_socket->can_read();
    if (m_local_socket)
        return m_local_socket->can_read();
    if (m_kqueue) {
        auto* process = Process::current();
        return process && m_kqueue->has_pending(*process);
    }
    if (m_port)
        return m_is_port_receiver && m_port->can_receive();
    if (m_pipe)
        return m_pipe->can_read();
    return m_inode && m_inode->can_read();
}

bool FileDescription::can_write() const
{
    if (m_socket)
        return m_socket->can_write();
    if (m_local_socket)
        return m_local_socket->can_write();
    if (m_kqueue)
        return false;
    if (m_port)
        return !m_is_port_receiver && m_port->can_send();
    if (m_pipe)
        return m_pipe->can_write();
    return m_inode && m_inode->can_write();
}

bool FileDescription::is_hung_up() const
{
    if (m_socket)
        return m_socket->is_hung_up();
    if (m_local_socket)
        return m_local_socket->is_hung_up();
    if (m_kqueue)
        return false;
    if (m_port)
        return m_is_port_receiver ? !m_port->has_senders() : !m_port->has_receiver();
    if (m_pipe) {
        if (m_is_pipe_both)
            return false;
        return m_is_pipe_writer ? !m_pipe->has_readers() : !m_pipe->has_writers();
    }
    return m_inode && m_inode->is_hung_up();
}

size_t FileDescription::bytes_to_read() const
{
    if (m_local_socket)
        return m_local_socket->readable_bytes();
    if (m_pipe)
        return m_pipe->buffered();
    if (m_socket || m_port || m_kqueue || !m_inode)
        return can_read() ? 1 : 0;
    auto const metadata = m_inode->metadata();
    if (metadata.type != InodeType::File)
        return can_read() ? 1 : 0;
    return metadata.size > m_offset ? metadata.size - m_offset : 0;
}

size_t FileDescription::room_to_write() const
{
    if (m_local_socket)
        return can_write() ? m_local_socket->buffer_size() : 0;
    if (m_pipe)
        return Pipe::capacity - min<size_t>(m_pipe->buffered(), Pipe::capacity);
    return can_write() ? 64 * KiB : 0;
}

void FileDescription::set_status_flags(int flags)
{
    constexpr int changeable = O_APPEND | O_NONBLOCK;
    m_flags = (m_flags & ~changeable) | (flags & changeable);
}

ErrorOr<size_t> FileDescription::read(IOBuffer const& buffer, size_t count)
{
    if (m_port || m_kqueue)
        return Error::from_errno(EINVAL); // Ports take port_receive().
    if (!is_readable())
        return Error::from_errno(EBADF);
    if (m_socket)
        return m_socket->receive(buffer, count, 0, nullptr, m_flags & O_NONBLOCK);
    if (m_local_socket) {
        LocalSocket::Rights rights;
        bool truncated = false;
        return m_local_socket->receive(buffer, count, m_flags & O_NONBLOCK, false, rights, truncated, nullptr);
    }
    if (m_pipe)
        return m_pipe->read(buffer, count, m_flags & O_NONBLOCK);
    auto metadata = m_inode->metadata();
    if (metadata.type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    // Devices have no offset to speak of.
    if (metadata.type == InodeType::CharacterDevice) {
        if ((m_flags & O_NONBLOCK) && !m_inode->can_read())
            return Error::from_errno(EAGAIN);
        return m_inode->read(0, count, buffer);
    }
    MutexLocker locker(m_lock);
    size_t done = TRY(m_inode->read(m_offset, count, buffer));
    m_offset += done;
    return done;
}

ErrorOr<size_t> FileDescription::write(IOBuffer const& buffer, size_t count)
{
    if (m_port || m_kqueue)
        return Error::from_errno(EINVAL); // Ports take port_send().
    if (!is_writable())
        return Error::from_errno(EBADF);
    if (m_local_socket) {
        auto sent = m_local_socket->send(buffer, count, m_flags & O_NONBLOCK, {}, {}, {});
        if (sent.is_error() && sent.error().code() == EPIPE && !m_local_socket->wants_no_sigpipe()) {
            if (auto* process = Process::current()) {
                vulture_siginfo info {};
                process->send_signal(SIGPIPE, info);
            }
        }
        return sent;
    }
    if (m_socket) {
        auto sent = m_socket->send(buffer, count, 0, nullptr, m_flags & O_NONBLOCK);
        if (sent.is_error() && sent.error().code() == EPIPE && !m_socket->wants_no_sigpipe()) {
            if (auto* process = Process::current()) {
                vulture_siginfo info {};
                process->send_signal(SIGPIPE, info);
            }
        }
        return sent;
    }
    if (m_pipe)
        return m_pipe->write(buffer, count, m_flags & O_NONBLOCK);
    auto metadata = m_inode->metadata();
    if (metadata.type == InodeType::CharacterDevice) {
        if ((m_flags & O_NONBLOCK) && !m_inode->can_write())
            return Error::from_errno(EAGAIN);
        return m_inode->write(0, count, buffer);
    }
    MutexLocker locker(m_lock);
    if (m_flags & O_APPEND)
        m_offset = m_inode->metadata().size;
    size_t done = TRY(m_inode->write(m_offset, count, buffer));
    m_offset += done;
    return done;
}

ErrorOr<size_t> FileDescription::read_at(IOBuffer const& buffer, size_t count, u64 offset)
{
    if (!is_readable())
        return Error::from_errno(EBADF);
    if (m_pipe || m_port || m_socket || m_local_socket || m_kqueue)
        return Error::from_errno(ESPIPE);
    auto metadata = m_inode->metadata();
    if (metadata.type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    if (metadata.type == InodeType::CharacterDevice)
        return Error::from_errno(ESPIPE);
    return m_inode->read(offset, count, buffer);
}

ErrorOr<size_t> FileDescription::write_at(IOBuffer const& buffer, size_t count, u64 offset)
{
    if (!is_writable())
        return Error::from_errno(EBADF);
    if (m_pipe || m_port || m_socket || m_local_socket || m_kqueue || m_inode->metadata().type == InodeType::CharacterDevice)
        return Error::from_errno(ESPIPE);
    return m_inode->write(offset, count, buffer);
}

ErrorOr<i64> FileDescription::seek(i64 offset, int origin)
{
    if (m_pipe || m_port || m_socket || m_local_socket || m_kqueue || m_inode->metadata().type == InodeType::CharacterDevice)
        return Error::from_errno(ESPIPE);
    MutexLocker locker(m_lock);
    i64 base = 0;
    switch (origin) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = static_cast<i64>(m_offset);
        break;
    case SEEK_END:
        base = static_cast<i64>(m_inode->metadata().size);
        break;
    default:
        return Error::from_errno(EINVAL);
    }
    i64 position;
    if (__builtin_add_overflow(base, offset, &position) || position < 0)
        return Error::from_errno(EINVAL);
    m_offset = static_cast<u64>(position);
    return position;
}

ErrorOr<vulture_stat> FileDescription::stat() const
{
    if (m_kqueue) {
        vulture_stat result {};
        result.st_mode = 0600;
        result.st_nlink = 1;
        result.st_blksize = 4096;
        return result;
    }
    if (m_port || m_socket || m_local_socket) {
        // A port shows as a socket would: not a file, not a pipe.
        vulture_stat result {};
        result.st_mode = S_IFSOCK | 0600;
        result.st_nlink = 1;
        result.st_blksize = 4096;
        return result;
    }
    if (m_pipe && m_inode) {
        auto result = m_inode->metadata().to_stat(m_inode->file_system().id());
        result.st_size = static_cast<long long>(m_pipe->buffered());
        return result;
    }
    if (m_pipe) {
        vulture_stat result {};
        result.st_mode = S_IFIFO | 0600;
        result.st_nlink = 1;
        result.st_size = static_cast<long long>(m_pipe->buffered());
        result.st_blksize = 4096;
        return result;
    }
    return m_inode->metadata().to_stat(m_inode->file_system().id());
}

ErrorOr<size_t> FileDescription::read_directory_entries(IOBuffer const& buffer, size_t size)
{
    if (!m_inode || !m_inode->is_directory())
        return Error::from_errno(ENOTDIR);
    MutexLocker locker(m_lock);

    // The offset counts entries. Records are gathered in a kernel buffer
    // first: traversal happens under filesystem locks, copying to user
    // memory must not.
    struct Collector final : DirectoryVisitor {
        u64 skip;
        u64 index { 0 };
        u8 records[4096];
        size_t used { 0 };
        size_t limit;
        bool full { false };

        VisitDecision visit(StringView name, InodeIndex inode, InodeType type) override
        {
            if (index < skip) {
                ++index;
                return VisitDecision::Continue;
            }
            size_t length = align_up<size_t>(sizeof(vulture_dirent) + name.length() + 1, 8);
            if (used + length > limit) {
                full = true;
                return VisitDecision::Stop;
            }
            auto* record = reinterpret_cast<vulture_dirent*>(records + used);
            __builtin_memset(record, 0, length);
            record->d_ino = inode;
            record->d_off = static_cast<long long>(index + 1);
            record->d_reclen = static_cast<unsigned short>(length);
            record->d_type = directory_entry_type(type);
            __builtin_memcpy(record->d_name, name.characters_without_null_termination(), name.length());
            used += length;
            ++index;
            return VisitDecision::Continue;
        }
    };
    auto* collector = new (Base::nothrow) Collector;
    if (!collector)
        return Error::from_errno(ENOMEM);
    collector->skip = m_offset;
    collector->limit = min(size, sizeof(collector->records));
    auto result = m_inode->traverse(*collector);
    size_t used = collector->used;
    u64 next = collector->index;
    if (!result.is_error() && used == 0 && collector->full)
        result = Error::from_errno(EINVAL); // Buffer too small for one record.
    if (!result.is_error())
        result = buffer.write(0, { collector->records, used });
    delete collector;
    TRY(result);
    m_offset = next;
    return used;
}

ErrorOr<void> FileDescription::truncate(u64 size)
{
    if (!m_inode || m_pipe || !is_writable())
        return Error::from_errno(EINVAL);
    return m_inode->truncate(size);
}

ErrorOr<void> FileDescription::sync()
{
    if (!m_inode)
        return {};
    TRY(m_inode->sync());
    return m_inode->file_system().sync();
}

}
