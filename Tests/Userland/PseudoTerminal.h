/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// A program on a pseudo-terminal of its own, typed at and read from the
// way a person at a terminal would, for the tests of interactive programs.
class PseudoTerminal {
public:
    // Starts `path` as a session leader with the terminal as its
    // controlling terminal and standard input, output and error.
    bool start(char const* path, char const* const* argv, char const* const* envp)
    {
        m_master = posix_openpt(O_RDWR | O_NOCTTY);
        if (m_master < 0 || grantpt(m_master) < 0 || unlockpt(m_master) < 0)
            return false;
        char slave_path[64];
        strncpy(slave_path, ptsname(m_master), sizeof(slave_path) - 1);
        slave_path[sizeof(slave_path) - 1] = '\0';
        m_child = fork();
        if (m_child == 0) {
            close(m_master);
            setsid();
            int slave = open(slave_path, O_RDWR);
            if (slave < 0)
                _exit(91);
            dup2(slave, 0);
            dup2(slave, 1);
            dup2(slave, 2);
            if (slave > 2)
                close(slave);
            execve(path, const_cast<char**>(argv), const_cast<char**>(envp));
            _exit(127);
        }
        return m_child > 0;
    }

    void type(char const* keys) { (void)write(m_master, keys, strlen(keys)); }

    static void pause_milliseconds(long milliseconds)
    {
        struct timespec duration { milliseconds / 1000, (milliseconds % 1000) * 1'000'000 };
        nanosleep(&duration, nullptr);
    }

    // Waits (up to `timeout` milliseconds) for `text` after what the last
    // expectation matched; on failure, shows what came instead.
    bool expect(char const* text, int timeout = 5000)
    {
        for (int waited = 0; waited < timeout;) {
            m_transcript[m_length] = '\0';
            if (char* found = strstr(m_transcript + m_position, text)) {
                m_position = static_cast<size_t>(found - m_transcript) + strlen(text);
                return true;
            }
            struct pollfd poller { m_master, POLLIN, 0 };
            if (poll(&poller, 1, 100) <= 0) {
                waited += 100;
                continue;
            }
            ssize_t got = read(m_master, m_transcript + m_length, sizeof(m_transcript) - 1 - m_length);
            if (got <= 0)
                break;
            m_length += static_cast<size_t>(got);
        }
        fprintf(stderr, "pty: never saw \"");
        print_visibly(text, strlen(text));
        fprintf(stderr, "\"; after it came:\n  \"");
        print_visibly(m_transcript + m_position, m_length - m_position);
        fprintf(stderr, "\"\n");
        return false;
    }

    // Whether `text` appears in what has come since the last expectation.
    bool saw(char const* text)
    {
        m_transcript[m_length] = '\0';
        return strstr(m_transcript + m_position, text) != nullptr;
    }

    // Waits for the program to end; its status, or -1 if it does not.
    int wait_for_exit(int timeout = 5000)
    {
        int status = -1;
        for (int waited = 0; waited < timeout; waited += 100) {
            if (waitpid(m_child, &status, WNOHANG) == m_child)
                return status;
            // Keep the output flowing, so the program never blocks on it.
            struct pollfd poller { m_master, POLLIN, 0 };
            if (poll(&poller, 1, 100) > 0) {
                ssize_t got = read(m_master, m_transcript + m_length, sizeof(m_transcript) - 1 - m_length);
                if (got > 0)
                    m_length += static_cast<size_t>(got);
            }
        }
        return -1;
    }

    ~PseudoTerminal()
    {
        if (m_master >= 0)
            close(m_master);
    }

    [[nodiscard]] pid_t child() const { return m_child; }

private:
    static void print_visibly(char const* text, size_t length)
    {
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            fprintf(stderr, c >= 0x20 && c < 0x7f ? "%c" : "\\x%02x", c);
        }
    }

    int m_master { -1 };
    pid_t m_child { -1 };
    char m_transcript[65536];
    size_t m_length { 0 };
    size_t m_position { 0 };
};
