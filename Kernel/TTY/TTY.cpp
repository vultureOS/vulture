/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/TTY/TTY.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

namespace {

constexpr vulture_cc_t control(char letter) { return static_cast<vulture_cc_t>(letter & 0x1f); }

bool is_special(u8 c, vulture_cc_t special)
{
    // 0 disables a special character (_POSIX_VDISABLE).
    return special != 0 && c == special;
}

}

TTY::TTY(u64 device_number, StringView name)
    : Device(InodeType::CharacterDevice, 0620, device_number)
{
    m_termios.c_iflag = ICRNL | IXON | IUTF8;
    m_termios.c_oflag = OPOST | ONLCR;
    m_termios.c_cflag = CS8 | CREAD | B38400;
    m_termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    m_termios.c_cc[VINTR] = control('C');
    m_termios.c_cc[VQUIT] = control('\\');
    m_termios.c_cc[VERASE] = 0x7f;
    m_termios.c_cc[VKILL] = control('U');
    m_termios.c_cc[VEOF] = control('D');
    m_termios.c_cc[VMIN] = 1;
    m_termios.c_cc[VSTART] = control('Q');
    m_termios.c_cc[VSTOP] = control('S');
    m_termios.c_cc[VSUSP] = control('Z');
    m_termios.c_cc[VREPRINT] = control('R');
    m_termios.c_cc[VWERASE] = control('W');
    m_termios.c_cc[VLNEXT] = control('V');
    m_termios.c_ispeed = m_termios.c_ospeed = B38400;
    m_name_length = min(name.length(), sizeof(m_name));
    __builtin_memcpy(m_name, name.characters_without_null_termination(), m_name_length);
}

bool TTY::is_readable_locked() const
{
    bool canonical = m_termios.c_lflag & ICANON;
    return m_ready_count > 0 || m_hung_up || (canonical && m_pending_end_of_file > 0) || (!canonical && m_termios.c_cc[VMIN] == 0);
}

bool TTY::can_read() const
{
    SpinLockGuard guard(m_lock);
    return is_readable_locked();
}

void TTY::commit_line(bool add_newline)
{
    if (add_newline && m_line_length < buffer_size)
        m_line[m_line_length++] = '\n';
    for (size_t i = 0; i < m_line_length && m_ready_count < buffer_size; ++i) {
        m_ready[(m_ready_head + m_ready_count) % buffer_size] = m_line[i];
        ++m_ready_count;
    }
    m_line_length = 0;
}

void TTY::receive(u8 c)
{
    char echo_buffer[64];
    size_t echo_length = 0;
    auto add_echo = [&](char character) {
        if (echo_length < sizeof(echo_buffer))
            echo_buffer[echo_length++] = character;
    };
    // How a character looks echoed: ^C for control characters.
    auto add_echo_visible = [&](u8 character, bool show_control) {
        if (show_control && (character < 0x20 || character == 0x7f) && character != '\n' && character != '\t') {
            add_echo('^');
            add_echo(character == 0x7f ? '?' : static_cast<char>(character + '@'));
        } else {
            add_echo(static_cast<char>(character));
        }
    };
    auto erase_echo = [&](bool echoing, bool visual) {
        if (echoing && visual) {
            add_echo('\b');
            add_echo(' ');
            add_echo('\b');
        }
    };
    int signal = 0;
    bool wake = false;
    {
        SpinLockGuard guard(m_lock);
        auto const& settings = m_termios;
        if (settings.c_iflag & ISTRIP)
            c &= 0x7f;
        if (c == '\r') {
            if (settings.c_iflag & IGNCR)
                return;
            if (settings.c_iflag & ICRNL)
                c = '\n';
        } else if (c == '\n' && (settings.c_iflag & INLCR)) {
            c = '\r';
        }
        bool echoing = settings.c_lflag & ECHO;
        bool show_control = settings.c_lflag & ECHOCTL;
        if (settings.c_lflag & ISIG) {
            if (is_special(c, settings.c_cc[VINTR]))
                signal = SIGINT;
            else if (is_special(c, settings.c_cc[VQUIT]))
                signal = SIGQUIT;
            else if (is_special(c, settings.c_cc[VSUSP]))
                signal = SIGTSTP;
        }
        if (signal) {
            if (!(settings.c_lflag & NOFLSH)) {
                m_line_length = 0;
                m_ready_count = 0;
                m_ready_head = 0;
            }
            if (echoing)
                add_echo_visible(c, show_control);
        } else if (settings.c_lflag & ICANON) {
            bool visual_erase = settings.c_lflag & ECHOE;
            if (is_special(c, settings.c_cc[VERASE]) || c == '\b') {
                if (m_line_length > 0) {
                    --m_line_length;
                    erase_echo(echoing, visual_erase);
                }
            } else if ((settings.c_lflag & IEXTEN) && is_special(c, settings.c_cc[VWERASE])) {
                while (m_line_length > 0 && m_line[m_line_length - 1] == ' ') {
                    --m_line_length;
                    erase_echo(echoing, visual_erase);
                }
                while (m_line_length > 0 && m_line[m_line_length - 1] != ' ') {
                    --m_line_length;
                    erase_echo(echoing, visual_erase);
                }
            } else if (is_special(c, settings.c_cc[VKILL])) {
                while (m_line_length > 0) {
                    --m_line_length;
                    erase_echo(echoing, (settings.c_lflag & ECHOKE) != 0);
                }
            } else if (is_special(c, settings.c_cc[VEOF])) {
                if (m_line_length == 0)
                    ++m_pending_end_of_file;
                else
                    commit_line(false);
                wake = true;
            } else if (c == '\n' || is_special(c, settings.c_cc[VEOL]) || is_special(c, settings.c_cc[VEOL2])) {
                commit_line(c == '\n');
                if (c != '\n') {
                    // VEOL ends the line and is kept.
                    if (m_ready_count < buffer_size) {
                        m_ready[(m_ready_head + m_ready_count) % buffer_size] = c;
                        ++m_ready_count;
                    }
                }
                if (echoing || (settings.c_lflag & ECHONL))
                    add_echo('\n');
                wake = true;
            } else if (m_line_length < buffer_size - 1) {
                m_line[m_line_length++] = c;
                if (echoing)
                    add_echo_visible(c, show_control);
            }
        } else {
            if (m_ready_count < buffer_size) {
                m_ready[(m_ready_head + m_ready_count) % buffer_size] = c;
                ++m_ready_count;
            }
            if (echoing)
                add_echo_visible(c, show_control);
            wake = true;
        }
    }
    if (echo_length > 0) {
        // Output processing applies to echo too (NL becomes CR NL).
        char processed[2 * sizeof(echo_buffer)];
        size_t length = 0;
        bool translate = (m_termios.c_oflag & OPOST) && (m_termios.c_oflag & ONLCR);
        for (size_t i = 0; i < echo_length; ++i) {
            if (translate && echo_buffer[i] == '\n')
                processed[length++] = '\r';
            processed[length++] = echo_buffer[i];
        }
        emit({ reinterpret_cast<u8 const*>(processed), length }, true);
    }
    if (signal)
        signal_foreground(signal);
    if (wake)
        m_read_wait.wake_all();
    notify_pollers();
}

void TTY::signal_foreground(int signal)
{
    ProcessID group = m_foreground_group.load();
    if (group <= 0)
        return;
    vulture_siginfo info {};
    info.si_code = SI_KERNEL;
    (void)Process::send_signal_to_group(group, signal, info);
}

ErrorOr<void> TTY::check_job_control(int signal)
{
    auto* process = Process::current();
    if (!process || process->controlling_terminal() != this)
        return {};
    ProcessID group = m_foreground_group.load();
    if (group == 0 || process->pgid() == group)
        return {};
    if (signal == SIGTTOU && !(m_termios.c_lflag & TOSTOP))
        return {};
    if (process->is_signal_ignored_or_blocked(signal)) {
        // Reading from the background with SIGTTIN ignored fails; writing
        // with SIGTTOU ignored goes ahead.
        if (signal == SIGTTIN)
            return Error::from_errno(EIO);
        return {};
    }
    vulture_siginfo info {};
    info.si_code = SI_KERNEL;
    (void)Process::send_signal_to_group(process->pgid(), signal, info);
    return Error::from_errno(EINTR);
}

ErrorOr<size_t> TTY::read(u64, size_t count, IOBuffer const& buffer)
{
    if (count == 0)
        return 0;
    TRY(check_job_control(SIGTTIN));
    for (;;) {
        u8 chunk[1024];
        size_t taken = 0;
        bool end_of_file = false;
        {
            SpinLockGuard guard(m_lock);
            bool canonical = m_termios.c_lflag & ICANON;
            if (m_ready_count > 0) {
                size_t limit = min(count, sizeof(chunk));
                while (taken < limit && m_ready_count > 0) {
                    u8 c = m_ready[m_ready_head];
                    m_ready_head = (m_ready_head + 1) % buffer_size;
                    --m_ready_count;
                    chunk[taken++] = c;
                    // One line per read in canonical mode.
                    if (canonical && c == '\n')
                        break;
                }
            } else if (canonical && m_pending_end_of_file > 0) {
                --m_pending_end_of_file;
                end_of_file = true;
            } else if (m_hung_up || (!canonical && m_termios.c_cc[VMIN] == 0)) {
                end_of_file = true;
            }
        }
        if (taken > 0) {
            TRY(buffer.write(0, { chunk, taken }));
            return taken;
        }
        if (end_of_file)
            return 0;
        if (!m_read_wait.wait_until_interruptible([&] { return can_read(); }))
            return Error::from_errno(EINTR);
    }
}

ErrorOr<size_t> TTY::write(u64, size_t count, IOBuffer const& buffer)
{
    TRY(check_job_control(SIGTTOU));
    size_t done = 0;
    while (done < count) {
        u8 chunk[512];
        size_t length = min(count - done, sizeof(chunk));
        TRY(buffer.read(done, { chunk, length }));
        u8 processed[2 * sizeof(chunk)];
        size_t processed_length = 0;
        bool translate = (m_termios.c_oflag & OPOST) && (m_termios.c_oflag & ONLCR);
        for (size_t i = 0; i < length; ++i) {
            if (translate && chunk[i] == '\n')
                processed[processed_length++] = '\r';
            processed[processed_length++] = chunk[i];
        }
        emit({ processed, processed_length }, false);
        done += length;
    }
    return done;
}

ErrorOr<FlatPtr> TTY::ioctl(FileDescription&, unsigned request, FlatPtr argument)
{
    auto* process = Process::current();
    switch (request) {
    case TCGETS: {
        vulture_termios settings;
        {
            SpinLockGuard guard(m_lock);
            settings = m_termios;
        }
        TRY(copy_to_user(argument, settings));
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        auto settings = TRY(copy_from_user<vulture_termios>(argument));
        bool wake = false;
        {
            SpinLockGuard guard(m_lock);
            if (request == TCSETSF) {
                m_ready_count = 0;
                m_ready_head = 0;
                m_line_length = 0;
            }
            // Leaving canonical mode: what was typed so far is input now.
            if ((m_termios.c_lflag & ICANON) && !(settings.c_lflag & ICANON) && m_line_length > 0) {
                commit_line(false);
                wake = true;
            }
            m_termios = settings;
            wake = wake || is_readable_locked();
        }
        if (wake)
            m_read_wait.wake_all();
        notify_pollers();
        return 0;
    }
    case TCFLSH: {
        if (argument == TCIFLUSH || argument == TCIOFLUSH) {
            SpinLockGuard guard(m_lock);
            m_ready_count = 0;
            m_ready_head = 0;
            m_line_length = 0;
        }
        return 0;
    }
    case TIOCGPGRP:
        TRY(copy_to_user(argument, static_cast<int>(m_foreground_group.load())));
        return 0;
    case TIOCSPGRP: {
        auto group = TRY(copy_from_user<int>(argument));
        if (!process || process->controlling_terminal() != this)
            return Error::from_errno(ENOTTY);
        if (group <= 0)
            return Error::from_errno(EINVAL);
        if (!Process::group_exists_in_session(group, m_session.load()))
            return Error::from_errno(EPERM);
        m_foreground_group.store(group);
        return 0;
    }
    case TIOCGSID:
        if (m_session.load() == 0)
            return Error::from_errno(ENOTTY);
        TRY(copy_to_user(argument, static_cast<int>(m_session.load())));
        return 0;
    case TIOCGWINSZ:
        TRY(copy_to_user(argument, window_size()));
        return 0;
    case TIOCSWINSZ:
        set_window_size(TRY(copy_from_user<vulture_winsize>(argument)));
        return 0;
    case TIOCSCTTY:
        if (!process)
            return Error::from_errno(EPERM);
        TRY(process->set_controlling_terminal(*this));
        return 0;
    case TIOCNOTTY:
        if (!process || process->controlling_terminal() != this)
            return Error::from_errno(ENOTTY);
        process->lose_controlling_terminal();
        return 0;
    case FIONREAD: {
        int count;
        {
            SpinLockGuard guard(m_lock);
            count = static_cast<int>(m_ready_count);
        }
        TRY(copy_to_user(argument, count));
        return 0;
    }
    default:
        return Error::from_errno(ENOTTY);
    }
}

vulture_winsize TTY::window_size() const
{
    SpinLockGuard guard(m_lock);
    return m_window;
}

void TTY::set_window_size(vulture_winsize size)
{
    bool changed;
    {
        SpinLockGuard guard(m_lock);
        changed = size.ws_row != m_window.ws_row || size.ws_col != m_window.ws_col;
        m_window = size;
    }
    if (changed)
        signal_foreground(SIGWINCH);
}

void TTY::set_session(ProcessID session, ProcessID foreground_group)
{
    m_session.store(session);
    m_foreground_group.store(foreground_group);
}

void TTY::release_session(ProcessID session)
{
    if (m_session.load() != session)
        return;
    // The session leader is gone: its foreground job is hung up.
    signal_foreground(SIGHUP);
    signal_foreground(SIGCONT);
    m_session.store(0);
    m_foreground_group.store(0);
}

bool TTY::is_hung_up() const
{
    SpinLockGuard guard(m_lock);
    return m_hung_up;
}

void TTY::hang_up()
{
    {
        SpinLockGuard guard(m_lock);
        m_hung_up = true;
    }
    m_read_wait.wake_all();
    notify_pollers();
    signal_foreground(SIGHUP);
    signal_foreground(SIGCONT);
}

}
