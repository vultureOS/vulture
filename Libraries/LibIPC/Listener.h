/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Message.h>

namespace IPC {

// Serves a receive right from the event loop: every message that arrives
// goes to the handler (typically an ipcc stub's handle()).
class Listener {
public:
    Listener(Core::EventLoop&, Port receive_right, Function<void(ReceivedMessage&)> handler);
    ~Listener();

    Listener(Listener const&) = delete;
    Listener& operator=(Listener const&) = delete;

    [[nodiscard]] Port const& port() const { return m_port; }

private:
    void drain();

    Core::EventLoop& m_loop;
    Port m_port;
    Function<void(ReceivedMessage&)> m_handler;
    int m_watch { 0 };
};

}
