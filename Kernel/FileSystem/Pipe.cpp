/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/Pipe.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Tasks/Process.h>
#include <LibBase/New.h>

namespace Kernel {

Pipe::Pipe(u8* buffer)
    : m_buffer(buffer)
{
}

ErrorOr<NonnullRefPtr<Pipe>> Pipe::create()
{
    auto* buffer = static_cast<u8*>(kmalloc(capacity));
    if (!buffer)
        return Error::from_errno(ENOMEM);
    auto* pipe = new (Base::nothrow) Pipe(buffer);
    if (!pipe) {
        kfree(buffer);
        return Error::from_errno(ENOMEM);
    }
    return adopt_ref(*pipe);
}

Pipe::~Pipe()
{
    kfree(m_buffer);
}

void Pipe::attach(bool writer)
{
    (writer ? m_writers : m_readers).fetch_add(1);
}

void Pipe::detach(bool writer)
{
    (writer ? m_writers : m_readers).fetch_sub(1);
    // The other side may be waiting for exactly this.
    m_readable.wake_all();
    m_writable.wake_all();
}

ErrorOr<size_t> Pipe::read(IOBuffer const& buffer, size_t count, bool nonblocking)
{
    if (count == 0)
        return 0;
    for (;;) {
        {
            MutexLocker locker(m_lock);
            size_t used = m_used.load();
            if (used > 0) {
                size_t total = min(count, used);
                size_t done = 0;
                // In up to two pieces, around the end of the ring.
                while (done < total) {
                    size_t chunk = min(total - done, capacity - m_read_position);
                    TRY(buffer.write(done, { m_buffer + m_read_position, chunk }));
                    m_read_position = (m_read_position + chunk) % capacity;
                    done += chunk;
                }
                m_used.fetch_sub(total);
                m_writable.wake_all();
                return total;
            }
            if (m_writers.load() == 0)
                return 0;
            if (nonblocking)
                return Error::from_errno(EAGAIN);
        }
        bool satisfied = m_readable.wait_until_interruptible([&] {
            return m_used.load() > 0 || m_writers.load() == 0;
        });
        if (!satisfied)
            return Error::from_errno(EINTR);
    }
}

ErrorOr<size_t> Pipe::write(IOBuffer const& buffer, size_t count, bool nonblocking)
{
    size_t written = 0;
    while (written < count) {
        {
            MutexLocker locker(m_lock);
            if (m_readers.load() == 0) {
                if (auto* process = Process::current()) {
                    vulture_siginfo info {};
                    process->send_signal(SIGPIPE, info);
                }
                if (written > 0)
                    return written;
                return Error::from_errno(EPIPE);
            }
            size_t space = capacity - m_used.load();
            if (space > 0) {
                size_t total = min(count - written, space);
                size_t write_position = (m_read_position + m_used.load()) % capacity;
                size_t done = 0;
                while (done < total) {
                    size_t chunk = min(total - done, capacity - write_position);
                    TRY(buffer.read(written + done, { m_buffer + write_position, chunk }));
                    write_position = (write_position + chunk) % capacity;
                    done += chunk;
                }
                m_used.fetch_add(total);
                written += total;
                m_readable.wake_all();
                continue;
            }
            if (nonblocking)
                return written > 0 ? ErrorOr<size_t>(written) : ErrorOr<size_t>(Error::from_errno(EAGAIN));
        }
        bool satisfied = m_writable.wait_until_interruptible([&] {
            return m_used.load() < capacity || m_readers.load() == 0;
        });
        if (!satisfied)
            return written > 0 ? ErrorOr<size_t>(written) : ErrorOr<size_t>(Error::from_errno(EINTR));
    }
    return written;
}

}
