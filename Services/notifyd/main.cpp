/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <NotifyEndpoint.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// notifyd: notifications by name (com.vulture.notifyd), for notify(3).
// A post reaches every registration for the name: a message to a port, or
// the registration's token written to a pipe. Each name also has a state
// (a 64-bit value) and the number of times it was posted, which
// notify_check compares.

namespace {

// Names under this prefix are the system's: only root sets their state.
constexpr StringView system_prefix = "com.vulture.system."_sv;

struct Registration {
    pid_t pid { 0 };
    i32 token { 0 };
    String name;
    IPC::Port port;   // A NotifyClient send right, or
    IPC::Port file;   // a pipe's write end.
};

struct Name {
    u64 state { 0 };
    u64 posts { 0 };
};

class NotifyService final : public NotifyStub {
public:
    NotifyService()
        : m_log(os_log_create("com.vulture.notifyd", "server"))
    {
    }

    void post(IPC::Sender const& sender, String name) override
    {
        if (name.is_empty())
            return;
        auto& entry = this->name(name);
        ++entry.posts;
        size_t told = 0;
        for (size_t i = 0; i < m_registrations.size();) {
            auto& registration = m_registrations[i];
            if (registration.name != name) {
                ++i;
                continue;
            }
            if (!deliver(registration)) {
                m_registrations.remove(i);
                continue;
            }
            ++told;
            ++i;
        }
        os_log_debug(m_log, "%s posted by %d, %zu registrations told", name.characters(), sender.pid, told);
    }

    i32 register_port(IPC::Sender const& sender, String name, IPC::Port port, i32 token) override
    {
        if (name.is_empty() || !port.is_valid())
            return EINVAL;
        cancel_registration(sender.pid, token);
        m_registrations.append({ sender.pid, token, move(name), move(port), {} });
        return 0;
    }

    i32 register_file(IPC::Sender const& sender, String name, IPC::Port file, i32 token) override
    {
        if (name.is_empty() || !file.is_valid())
            return EINVAL;
        // notifyd never waits for a client: a full pipe misses the post.
        int flags = fcntl(file.fd(), F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY)
            return EBADF;
        fcntl(file.fd(), F_SETFL, flags | O_NONBLOCK);
        cancel_registration(sender.pid, token);
        m_registrations.append({ sender.pid, token, move(name), {}, move(file) });
        return 0;
    }

    void cancel(IPC::Sender const& sender, i32 token) override { cancel_registration(sender.pid, token); }

    i32 set_state(IPC::Sender const& sender, String name, u64 state) override
    {
        if (name.is_empty())
            return EINVAL;
        if (name.view().starts_with(system_prefix) && sender.uid != 0) {
            os_log_error(m_log, "%d (uid %u) may not set the state of %s", sender.pid, sender.uid, name.characters());
            return EPERM;
        }
        this->name(name).state = state;
        return 0;
    }

    u64 get_state(IPC::Sender const&, String name) override
    {
        auto const* entry = m_names.get(name);
        return entry ? entry->state : 0;
    }

    u64 post_count(IPC::Sender const&, String name) override
    {
        auto const* entry = m_names.get(name);
        return entry ? entry->posts : 0;
    }

private:
    Name& name(String const& name)
    {
        if (auto* entry = m_names.get(name))
            return *entry;
        m_names.set(name, {});
        return *m_names.get(name);
    }

    void cancel_registration(pid_t pid, i32 token)
    {
        for (size_t i = 0; i < m_registrations.size(); ++i) {
            if (m_registrations[i].pid == pid && m_registrations[i].token == token) {
                m_registrations.remove(i);
                return;
            }
        }
    }

    // False once the registration's client has gone.
    bool deliver(Registration& registration)
    {
        if (registration.port.is_valid()) {
            IPC::Encoder encoder(NotifyClientEndpoint::magic, static_cast<u32>(NotifyClientEndpoint::Method::Notification));
            encoder.encode(registration.token);
            encoder.encode(registration.name.view());
            auto sent = IPC::send(registration.port, encoder, true);
            return !sent.is_error() || sent.error().code() == EAGAIN;
        }
        // The token, big-endian, as notify_register_file_descriptor's
        // readers expect.
        u32 token = static_cast<u32>(registration.token);
        u8 bytes[4] = { static_cast<u8>(token >> 24), static_cast<u8>(token >> 16), static_cast<u8>(token >> 8), static_cast<u8>(token) };
        ssize_t written = write(registration.file.fd(), bytes, sizeof(bytes));
        return written == sizeof(bytes) || (written < 0 && errno == EAGAIN);
    }

    os_log_t m_log;
    HashMap<String, Name> m_names;
    Vector<Registration> m_registrations;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.notifyd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "notifyd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    NotifyService service;
    os_log(os_log_create("com.vulture.notifyd", "server"), "Starting");
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
