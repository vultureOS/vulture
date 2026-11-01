/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <EchoEndpoint.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The Echo service (Echo.ipc), started by vlaunchd when a message comes
// for com.vulture.test.echo (see bootstrap.cpp).

namespace {

class EchoService final : public EchoStub {
public:
    explicit EchoService(Core::EventLoop& loop)
        : m_loop(loop)
    {
    }

    String echo(IPC::Sender const&, String text) override { return text; }

    void shout(IPC::Sender const&, String text) override
    {
        int fd = open("/tmp/bootstrap-test/shouts", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            return;
        (void)write(fd, text.characters(), text.length());
        (void)write(fd, "\n", 1);
        close(fd);
    }

    i32 process(IPC::Sender const&) override { return getpid(); }

    EchoEndpoint::LengthsAnswer lengths(IPC::Sender const&, Vector<String> words) override
    {
        EchoEndpoint::LengthsAnswer answer;
        for (auto const& word : words) {
            answer.lengths.append(static_cast<u32>(word.length()));
            answer.total += word.length();
        }
        return answer;
    }

    void stop(IPC::Sender const&) override { m_loop.quit(0); }

private:
    Core::EventLoop& m_loop;
};

}

int main()
{
    auto port = IPC::check_in("com.vulture.test.echo"_sv);
    if (port.is_error()) {
        fprintf(stderr, "echo-service: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    EchoService service(loop);
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
