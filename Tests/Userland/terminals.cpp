/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// Pseudo-terminals, the line discipline and job control, driven from the
// master side as a terminal window would.

namespace {

struct Terminal {
    int master;
    char slave_path[32];
};

Terminal open_terminal()
{
    Terminal terminal {};
    terminal.master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(terminal.master >= 0);
    CHECK(grantpt(terminal.master) == 0 && unlockpt(terminal.master) == 0);
    char const* name = ptsname(terminal.master);
    CHECK(name != nullptr && strncmp(name, "/dev/ttys", 9) == 0);
    strncpy(terminal.slave_path, name ? name : "", sizeof(terminal.slave_path) - 1);
    return terminal;
}

// In the child: a new session with the terminal as its controlling
// terminal and standard input, output and error.
void become_session_leader(Terminal const& terminal)
{
    close(terminal.master);
    if (setsid() < 0)
        _exit(90);
    int slave = open(terminal.slave_path, O_RDWR);
    if (slave < 0)
        _exit(91);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
        close(slave);
}

// Reads from the master until `text` has appeared (the test times out
// otherwise).
bool read_until(int master, char const* text)
{
    static char seen[8192];
    size_t length = 0;
    while (length < sizeof(seen) - 1) {
        ssize_t got = read(master, seen + length, sizeof(seen) - 1 - length);
        if (got <= 0)
            return false;
        length += static_cast<size_t>(got);
        seen[length] = '\0';
        if (strstr(seen, text))
            return true;
    }
    return false;
}

void test_canonical_mode_and_echo()
{
    auto terminal = open_terminal();
    pid_t child = fork();
    if (child == 0) {
        become_session_leader(terminal);
        char line[128];
        ssize_t length = read(0, line, sizeof(line) - 1);
        if (length <= 0)
            _exit(92);
        line[length] = '\0';
        dprintf(1, "got: %s", line);
        // The terminal is ours, and we lead its foreground group.
        _exit(tcgetpgrp(0) == getpid() && tcgetsid(0) == getpid() ? 0 : 93);
    }
    // "hellp", Backspace, "o world", Return: one line, edited.
    CHECK(write(terminal.master, "hellp\x7fo world\r", 14) == 14);
    CHECK(read_until(terminal.master, "got: hello world\r\n"));
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(terminal.master);
}

void test_interrupt_character()
{
    auto terminal = open_terminal();
    pid_t child = fork();
    if (child == 0) {
        become_session_leader(terminal);
        dprintf(1, "ready\n");
        char byte;
        (void)read(0, &byte, 1);
        _exit(94);
    }
    CHECK(read_until(terminal.master, "ready"));
    CHECK(write(terminal.master, "\x03", 1) == 1);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
    close(terminal.master);
}

void test_suspend_and_continue()
{
    auto terminal = open_terminal();
    pid_t child = fork();
    if (child == 0) {
        become_session_leader(terminal);
        dprintf(1, "ready\n");
        char line[64];
        ssize_t length;
        // A read cut short by the stop comes back with EINTR.
        while ((length = read(0, line, sizeof(line))) < 0 && errno == EINTR) {
        }
        _exit(length == 5 && memcmp(line, "back\n", 5) == 0 ? 0 : 95);
    }
    CHECK(read_until(terminal.master, "ready"));
    // ^Z stops the foreground job.
    CHECK(write(terminal.master, "\x1a", 1) == 1);
    int status = 0;
    CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTSTP);
    CHECK(kill(child, SIGCONT) == 0);
    CHECK(waitpid(child, &status, WCONTINUED) == child && WIFCONTINUED(status));
    CHECK(write(terminal.master, "back\n", 5) == 5);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(terminal.master);
}

void test_raw_mode_and_window_size()
{
    auto terminal = open_terminal();
    struct winsize size { 40, 132, 0, 0 };
    CHECK(ioctl(terminal.master, TIOCSWINSZ, &size) == 0);
    pid_t child = fork();
    if (child == 0) {
        become_session_leader(terminal);
        struct termios settings;
        if (tcgetattr(0, &settings) < 0 || !(settings.c_lflag & ICANON))
            _exit(96);
        cfmakeraw(&settings);
        if (tcsetattr(0, TCSANOW, &settings) < 0)
            _exit(97);
        struct winsize seen {};
        if (ioctl(0, TIOCGWINSZ, &seen) < 0 || seen.ws_row != 40 || seen.ws_col != 132)
            _exit(98);
        dprintf(1, "raw\n");
        char byte = 0;
        if (read(0, &byte, 1) != 1)
            _exit(99);
        dprintf(1, "[%c]", byte);
        _exit(0);
    }
    CHECK(read_until(terminal.master, "raw"));
    // No newline needed, and no echo.
    CHECK(write(terminal.master, "x", 1) == 1);
    CHECK(read_until(terminal.master, "[x]"));
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(terminal.master);
}

void test_background_read_stops()
{
    auto terminal = open_terminal();
    pid_t child = fork();
    if (child == 0) {
        become_session_leader(terminal);
        pid_t background = fork();
        if (background == 0) {
            // Not the foreground group: reading the terminal stops us.
            setpgid(0, 0);
            char byte;
            (void)read(0, &byte, 1);
            _exit(0);
        }
        setpgid(background, background);
        int status = 0;
        pid_t result = waitpid(background, &status, WUNTRACED);
        bool stopped = result == background && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN;
        kill(background, SIGKILL);
        waitpid(background, &status, 0);
        _exit(stopped ? 0 : 100);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(terminal.master);
}

}

int main()
{
    CHECK(getpgrp() > 0 && getsid(0) > 0);
    test_canonical_mode_and_echo();
    test_interrupt_character();
    test_suspend_and_continue();
    test_raw_mode_and_window_size();
    test_background_read_stops();
    // The terminal is gone with its master.
    errno = 0;
    CHECK(open("/dev/ttys000", O_RDWR) < 0 && errno == ENOENT);
    return finish();
}
