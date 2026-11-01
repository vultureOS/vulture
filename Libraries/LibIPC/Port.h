/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/StdLibExtras.h>

namespace IPC {

// A descriptor this object owns and closes: a right to a port, or any
// open file being sent along.
class Port {
public:
    Port() = default;
    explicit Port(int fd)
        : m_fd(fd)
    {
    }
    Port(Port&& other)
        : m_fd(exchange(other.m_fd, -1))
    {
    }
    Port& operator=(Port&& other);
    Port(Port const&) = delete;
    Port& operator=(Port const&) = delete;
    ~Port();

    [[nodiscard]] int fd() const { return m_fd; }
    [[nodiscard]] bool is_valid() const { return m_fd >= 0; }
    // Gives the descriptor up (the caller closes it now).
    [[nodiscard]] int release() { return exchange(m_fd, -1); }

    // Another descriptor for the same thing.
    [[nodiscard]] ErrorOr<Port> duplicate() const;
    // A send right made from this receive right.
    [[nodiscard]] ErrorOr<Port> make_send() const;

private:
    int m_fd { -1 };
};

struct PortPair {
    Port receive;
    Port send;
};

// A new port: its receive right and a send right (closed on exec).
ErrorOr<PortPair> create_port();

}
