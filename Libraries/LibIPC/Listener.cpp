/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Listener.h>
#include <errno.h>

namespace IPC {

Listener::Listener(Core::EventLoop& loop, Port receive_right, Function<void(ReceivedMessage&)> handler)
    : m_loop(loop)
    , m_port(move(receive_right))
    , m_handler(move(handler))
{
    m_watch = m_loop.watch(m_port.fd(), Core::EventLoop::Interest::Read, [this] { drain(); });
}

Listener::~Listener()
{
    m_loop.unwatch(m_watch);
}

void Listener::drain()
{
    // Everything waiting, then back to the loop. With no sender left the
    // port reads as ready forever: stop watching it. Once the program is
    // quitting, what is left waits for whoever serves the port next.
    while (!m_loop.is_quitting()) {
        auto message = receive(m_port, true);
        if (message.is_error()) {
            if (message.error().code() == EPIPE) {
                m_loop.unwatch(m_watch);
                m_watch = 0;
            }
            return;
        }
        m_handler(message.value());
    }
}

}
