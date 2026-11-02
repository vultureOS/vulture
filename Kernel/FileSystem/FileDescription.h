/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Locking/Mutex.h>

namespace Kernel {

class Pipe;
class Port;
namespace Net {
class Socket;
}

// An open file: what a file descriptor points to. Descriptors duplicated
// by dup() or fork() share one description, and with it the file offset.
class FileDescription final : public RefCounted<FileDescription> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<FileDescription>> create(NonnullRefPtr<Inode>, int flags);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<FileDescription>> create_for_pipe(NonnullRefPtr<Pipe>, bool is_writer);
    // A right to a port: the receive right, or a send right.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<FileDescription>> create_for_port(NonnullRefPtr<Port>, bool receive_right);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<FileDescription>> create_for_socket(NonnullRefPtr<Net::Socket>);
    ~FileDescription();

    ErrorOr<size_t> read(IOBuffer const&, size_t count);
    ErrorOr<size_t> write(IOBuffer const&, size_t count);
    // pread and pwrite: at `offset`, leaving the file offset as it is.
    ErrorOr<size_t> read_at(IOBuffer const&, size_t count, u64 offset);
    ErrorOr<size_t> write_at(IOBuffer const&, size_t count, u64 offset);
    ErrorOr<i64> seek(i64 offset, int origin);
    [[nodiscard]] ErrorOr<vulture_stat> stat() const;
    // getdents(): packed vulture_dirent records, as many as fit.
    ErrorOr<size_t> read_directory_entries(IOBuffer const&, size_t size);
    ErrorOr<void> truncate(u64 size);
    ErrorOr<void> sync();

    // For poll().
    [[nodiscard]] bool can_read() const;
    [[nodiscard]] bool can_write() const;
    [[nodiscard]] bool is_hung_up() const;

    [[nodiscard]] int flags() const { return m_flags; }
    void set_status_flags(int flags);
    [[nodiscard]] Inode* inode() const { return m_inode.ptr(); }
    [[nodiscard]] Port* port() const { return m_port.ptr(); }
    [[nodiscard]] Net::Socket* socket() const { return m_socket.ptr(); }
    [[nodiscard]] bool is_port_receive_right() const { return m_port && m_is_port_receiver; }
    [[nodiscard]] bool is_port_send_right() const { return m_port && !m_is_port_receiver; }
    [[nodiscard]] bool is_readable() const { return (m_flags & O_ACCMODE) != O_WRONLY; }
    [[nodiscard]] bool is_writable() const { return (m_flags & O_ACCMODE) != O_RDONLY; }

private:
    FileDescription(int flags);

    RefPtr<Inode> m_inode;
    RefPtr<Pipe> m_pipe;
    bool m_is_pipe_writer { false };
    RefPtr<Port> m_port;
    bool m_is_port_receiver { false };
    RefPtr<Net::Socket> m_socket;
    int m_flags { 0 };
    mutable Mutex m_lock;
    u64 m_offset { 0 };
};

}
