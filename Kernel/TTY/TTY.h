/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/termios.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/Locking/WaitQueue.h>

namespace Kernel {

using ProcessID = i32;

// A terminal: the line discipline between a keyboard-and-screen (the
// console, or a pseudo-terminal's master side) and the programs reading
// and writing it.
//
// - Canonical mode collects a line, with erase (Backspace), word erase
//   (^W), line kill (^U) and end of file (^D), before a read returns it.
//   Raw mode hands over bytes as they come.
// - Echo, and the input and output translations (CR to NL, NL to CR NL).
// - ^C, ^\ and ^Z send SIGINT, SIGQUIT and SIGTSTP to the foreground
//   process group.
// - Job control: a terminal belongs to one session. Background groups
//   that read it get SIGTTIN (and SIGTTOU for writes, with TOSTOP).
//
// Input arrives through receive(), always in thread context.
class TTY : public Device {
public:
    ~TTY() override = default;

    ErrorOr<size_t> read(u64 offset, size_t count, IOBuffer const&) override;
    ErrorOr<size_t> write(u64 offset, size_t count, IOBuffer const&) override;
    ErrorOr<FlatPtr> ioctl(FileDescription&, unsigned request, FlatPtr argument) override;
    [[nodiscard]] TTY* as_tty() override { return this; }
    [[nodiscard]] bool can_read() const override;
    [[nodiscard]] bool can_write() const override { return true; }

    // Input from the keyboard side.
    void receive(u8);
    void receive(ReadonlyBytes bytes)
    {
        for (u8 byte : bytes)
            receive(byte);
    }

    [[nodiscard]] ProcessID session() const { return m_session.load(); }
    [[nodiscard]] ProcessID foreground_group() const { return m_foreground_group.load(); }
    // A session takes the terminal as its controlling terminal.
    void set_session(ProcessID session, ProcessID foreground_group);
    void release_session(ProcessID session);
    // The other side went away: readers see end of file, the session
    // gets SIGHUP.
    void hang_up();

    [[nodiscard]] vulture_winsize window_size() const;
    void set_window_size(vulture_winsize);
    [[nodiscard]] StringView tty_name() const { return { m_name, m_name_length }; }

protected:
    TTY(u64 device_number, StringView name);

    // Output to the screen side: echo and what programs write. May block
    // (a pseudo-terminal's buffer is full) unless `from_echo`.
    virtual void emit(ReadonlyBytes, bool from_echo) = 0;

private:
    void echo(u8);
    void echo(StringView text) { emit(text.bytes(), true); }
    void commit_line(bool add_newline);
    void signal_foreground(int signal);
    [[nodiscard]] bool is_readable_locked() const;
    // A background job touching the terminal: SIGTTIN or SIGTTOU to its
    // group, and EINTR. Nothing when it may go ahead.
    ErrorOr<void> check_job_control(int signal);

    mutable SpinLock m_lock;
    vulture_termios m_termios {};
    vulture_winsize m_window { 25, 80, 0, 0 };

    // Bytes ready for read(): whole lines in canonical mode.
    static constexpr size_t buffer_size = 4096;
    u8 m_ready[buffer_size];
    size_t m_ready_head { 0 };
    size_t m_ready_count { 0 };
    // Canonical mode: the line being typed.
    u8 m_line[buffer_size];
    size_t m_line_length { 0 };
    // ^D on an empty line: the next read returns 0 (end of file).
    size_t m_pending_end_of_file { 0 };
    bool m_hung_up { false };
    WaitQueue m_read_wait;

    Atomic<ProcessID> m_session { 0 };
    Atomic<ProcessID> m_foreground_group { 0 };
    char m_name[16] {};
    size_t m_name_length { 0 };
};

}
