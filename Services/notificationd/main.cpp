/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibBase/OwnPtr.h>
#include <LibJSON/JSON.h>
#include <NotificationsEndpoint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

// com.vulture.usernoted: what programs post goes to whoever shows it (the banners and
// Notification Center), and stays until cleared.

namespace {

constexpr size_t kept_at_most = 64;

struct Note {
    String application;
    String title;
    String body;
};

class Center final : public NotificationCenterStub {
public:
    void post(IPC::Sender const&, String application, String title, String body) override
    {
        for (size_t i = 0; i < m_listeners.size();) {
            if (m_listeners[i]->posted(application.view(), title.view(), body.view()).is_error()) {
                m_listeners.remove(i);
                continue;
            }
            ++i;
        }
        m_notes.insert(0, Note { move(application), move(title), move(body) });
        if (m_notes.size() > kept_at_most)
            (void)m_notes.take_last();
    }

    String all(IPC::Sender const&) override
    {
        auto list = JSON::Value::array();
        for (auto const& note : m_notes) {
            auto one = JSON::Value::object();
            MUST(one.set("application"_sv, MUST(JSON::Value::string(note.application.view()))));
            MUST(one.set("title"_sv, MUST(JSON::Value::string(note.title.view()))));
            MUST(one.set("body"_sv, MUST(JSON::Value::string(note.body.view()))));
            MUST(list.append(move(one)));
        }
        return MUST(list.serialize());
    }

    void clear(IPC::Sender const&) override { m_notes.clear(); }

    void subscribe(IPC::Sender const&, IPC::Port events) override { m_listeners.append(adopt_own(*new NotificationEventsProxy(move(events)))); }

private:
    Vector<Note> m_notes;
    Vector<NonnullOwnPtr<NotificationEventsProxy>> m_listeners;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.usernoted"_sv);
    if (port.is_error()) {
        fprintf(stderr, "usernoted: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    Center center;
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)center.handle(message); });
    return loop.exec();
}
