/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>
#include <Kernel/FileSystem/IOBuffer.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <LibBase/RefPtr.h>

namespace Kernel {

// A one-way channel between processes, with a 64 KiB buffer. Readers
// wait while it is empty and writers remain; writers wait while it is
// full and readers remain. Reading with no writers left is end of file;
// writing with no readers left is EPIPE (and SIGPIPE).
class Pipe final : public RefCounted<Pipe> {
public:
    static constexpr size_t capacity = 64 * KiB;

    [[nodiscard]] static ErrorOr<NonnullRefPtr<Pipe>> create();
    // The pipe behind a named one, for as long as someone has it open.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Pipe>> for_fifo(Inode&);
    static void forget_fifo_if_unused(Inode&, Pipe&);
    ~Pipe();

    ErrorOr<size_t> read(IOBuffer const&, size_t count, bool nonblocking);
    ErrorOr<size_t> write(IOBuffer const&, size_t count, bool nonblocking);

    void attach(bool writer);
    void detach(bool writer);

    [[nodiscard]] size_t buffered() const { return m_used.load(); }
    // For poll(): data or end of file to read; room (or no reader) to write.
    [[nodiscard]] bool can_read() const { return m_used.load() > 0 || m_writers.load() == 0; }
    [[nodiscard]] bool can_write() const { return m_used.load() < capacity || m_readers.load() == 0; }
    [[nodiscard]] bool has_writers() const { return m_writers.load() > 0; }
    [[nodiscard]] bool has_readers() const { return m_readers.load() > 0; }
    // Opening a named pipe waits for the other end, which may have come and gone by the time it looks. So: how many there have ever
    // been, from before it opened; its own end opened; then this, with that number. False if a signal came first.
    [[nodiscard]] u64 times_other_end_opened(bool writer) const { return (writer ? m_reader_opens : m_writer_opens).load(); }
    [[nodiscard]] bool wait_for_other_end(bool writer, u64 seen);

private:
    Pipe(u8* buffer);

    Mutex m_lock;
    u8* m_buffer;
    size_t m_read_position { 0 };
    Atomic<size_t> m_used { 0 };
    Atomic<u32> m_readers { 0 };
    Atomic<u32> m_writers { 0 };
    WaitQueue m_readable;
    WaitQueue m_writable;
    WaitQueue m_other_end;
    Atomic<u64> m_reader_opens { 0 };
    Atomic<u64> m_writer_opens { 0 };
};

}
