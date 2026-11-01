/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCore/GetPassword.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

namespace Core {

namespace {

volatile sig_atomic_t s_caught_signal = 0;

void catch_signal(int signal)
{
    s_caught_signal = signal;
}

}

ErrorOr<String> get_password(StringView prompt)
{
    int fd = open("/dev/tty", O_RDWR);
    bool own_fd = fd >= 0;
    if (!own_fd)
        fd = STDIN_FILENO;
    int output = own_fd ? fd : STDERR_FILENO;

    struct termios original;
    bool terminal = tcgetattr(fd, &original) == 0;
    if (terminal) {
        struct termios quiet = original;
        quiet.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
        quiet.c_lflag |= ICANON;
        tcsetattr(fd, TCSAFLUSH, &quiet);
    }
    // ^C (or ^\\, ^Z) must not leave the terminal silent: the signal is
    // caught, the terminal put back, and then the signal raised again.
    s_caught_signal = 0;
    struct sigaction catcher {}, old_interrupt {}, old_quit {}, old_stop {};
    catcher.sa_handler = catch_signal;
    sigaction(SIGINT, &catcher, &old_interrupt);
    sigaction(SIGQUIT, &catcher, &old_quit);
    sigaction(SIGTSTP, &catcher, &old_stop);

    (void)write(output, prompt.characters_without_null_termination(), prompt.length());
    char buffer[256];
    size_t length = 0;
    bool ended = false;
    for (;;) {
        char c;
        ssize_t got = read(fd, &c, 1);
        if (got < 0 && errno == EINTR && !s_caught_signal)
            continue;
        if (got <= 0) {
            ended = length == 0;
            break;
        }
        if (c == '\n' || c == '\r')
            break;
        if (length < sizeof(buffer) - 1)
            buffer[length++] = c;
    }
    (void)write(output, "\n", 1);

    if (terminal)
        tcsetattr(fd, TCSAFLUSH, &original);
    sigaction(SIGINT, &old_interrupt, nullptr);
    sigaction(SIGQUIT, &old_quit, nullptr);
    sigaction(SIGTSTP, &old_stop, nullptr);
    if (own_fd)
        close(fd);

    auto result = String::from_view(StringView(buffer, length));
    __builtin_memset(buffer, 0, sizeof(buffer));
    if (int signal = s_caught_signal) {
        raise(signal);
        return Error::from_errno(EINTR);
    }
    if (ended)
        return Error::from_errno(EIO);
    return result;
}

}
