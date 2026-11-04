/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Locking/Mutex.h>
#include <LibBase/String.h>

namespace Kernel {

class KQueue;
class LocalSocket;
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
    [[nodiscard]] static ErrorOr<NonnullRefPtr<FileDescription>> create_for_local_socket(NonnullRefPtr<LocalSocket>);
    [[nodiscard]] static ErrorOr<NonnullRefPtr<FileDescription>> create_for_kqueue(NonnullRefPtr<KQueue>);
    // A named pipe: the file it has as a name, and the pipe that's behind it for now.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<FileDescription>> create_for_fifo(NonnullRefPtr<Inode>, NonnullRefPtr<Pipe>, int flags);
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
    // For kqueue's data, and FIONREAD: how much could be read now, or written.
    [[nodiscard]] size_t bytes_to_read() const;
    [[nodiscard]] size_t room_to_write() const;

    [[nodiscard]] int flags() const { return m_flags; }
    void set_status_flags(int flags);
    [[nodiscard]] Inode* inode() const { return m_inode.ptr(); }
    [[nodiscard]] Port* port() const { return m_port.ptr(); }
    [[nodiscard]] Net::Socket* socket() const { return m_socket.ptr(); }
    [[nodiscard]] LocalSocket* local_socket() const { return m_local_socket.ptr(); }
    [[nodiscard]] KQueue* kqueue() const { return m_kqueue.ptr(); }
    [[nodiscard]] bool is_pipe() const { return !m_pipe.is_null(); }
    // The file offset, for those that have one.
    [[nodiscard]] u64 offset() const { return m_offset; }
    // Where it was opened: what's relative to it (openat), and F_GETPATH. Empty if it wasn't by name.
    [[nodiscard]] StringView path() const { return m_path.view(); }
    // Something was written through this: FSEvents hears of it, though not as often as it happens.
    void note_written();
    void set_path(String path) { m_path = move(path); }
    // Different for every description there has ever been, unlike an address.
    [[nodiscard]] u64 serial() const { return m_serial; }
    [[nodiscard]] bool is_port_receive_right() const { return m_port && m_is_port_receiver; }
    [[nodiscard]] bool is_port_send_right() const { return m_port && !m_is_port_receiver; }
    [[nodiscard]] bool is_readable() const { return (m_flags & O_ACCMODE) != O_WRONLY; }
    [[nodiscard]] bool is_writable() const { return (m_flags & O_ACCMODE) != O_RDONLY; }

private:
    FileDescription(int flags);

    RefPtr<Inode> m_inode;
    RefPtr<Pipe> m_pipe;
    bool m_is_pipe_writer { false };
    bool m_is_pipe_both { false };
    RefPtr<Port> m_port;
    bool m_is_port_receiver { false };
    RefPtr<Net::Socket> m_socket;
    RefPtr<LocalSocket> m_local_socket;
    RefPtr<KQueue> m_kqueue;
    String m_path;
    u64 m_last_written_event { 0 };
    bool m_written_unreported { false };
    u64 m_serial { 0 };
    int m_flags { 0 };
    mutable Mutex m_lock;
    u64 m_offset { 0 };
};

}
