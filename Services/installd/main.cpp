/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <InstallerEndpoint.h>
#include <LibBase/OwnPtr.h>
#include <LibCore/Account.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibPackage/Manager.h>
#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// com.vulture.installd: anyone may ask what there is; only an administrator may change it.
//
// Each job runs in a process of its own, telling this one how it goes over a pipe. A download
// takes as long as it takes, and everything in it (a certificate, a server's HTTP, a tar file)
// comes from outside: if it goes wrong, it goes wrong over there.

namespace {

enum class JobKind {
    Refresh,
    Install,
    Remove,
};

struct Job {
    JobKind kind;
    String name;
};

class Installer final : public InstallerStub {
public:
    explicit Installer(Core::EventLoop& loop)
        : m_loop(loop)
        , m_log(os_log_create("com.vulture.installd", "jobs"))
    {
    }

    String catalog(IPC::Sender const&) override { return Package::describe_catalog(); }

    void refresh(IPC::Sender const& sender) override
    {
        if (!may_change_things(sender)) {
            refused({}, sender);
            return;
        }
        enqueue({ JobKind::Refresh, {} });
    }

    void install(IPC::Sender const& sender, String name) override
    {
        if (!may_change_things(sender)) {
            refused(name.view(), sender);
            return;
        }
        enqueue({ JobKind::Install, move(name) });
    }

    void remove(IPC::Sender const& sender, String name) override
    {
        if (!may_change_things(sender)) {
            refused(name.view(), sender);
            return;
        }
        enqueue({ JobKind::Remove, move(name) });
    }

    void subscribe(IPC::Sender const&, IPC::Port events) override
    {
        m_listeners.append(adopt_own(*new InstallerEventsProxy(move(events))));
    }

private:
    bool may_change_things(IPC::Sender const& sender)
    {
        if (sender.uid == 0)
            return true;
        auto account = Core::Account::from_uid(sender.uid);
        return !account.is_error() && account.value().is_in_group("admin"_sv);
    }

    void refused(StringView name, IPC::Sender const& sender)
    {
        os_log(m_log, "Process %d isn't an administrator's", static_cast<int>(sender.pid));
        tell([&](InstallerEventsProxy& listener) { return listener.finished(name, false, "Only an administrator can change what is installed."_sv); });
    }

    template<typename Call>
    void tell(Call call)
    {
        for (size_t i = 0; i < m_listeners.size();) {
            if (call(*m_listeners[i]).is_error()) {
                m_listeners.remove(i);
                continue;
            }
            ++i;
        }
    }

    void enqueue(Job job)
    {
        m_jobs.append(move(job));
        run_next();
    }

    void run_next()
    {
        if (m_worker > 0 || m_jobs.is_empty())
            return;
        m_current = move(m_jobs.first());
        m_jobs.remove(0);

        int fds[2];
        if (pipe(fds) != 0) {
            finished_job(false, "Could not start: out of resources."_sv);
            return;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(fds[0]);
            close(fds[1]);
            finished_job(false, "Could not start: out of resources."_sv);
            return;
        }
        if (pid == 0) {
            close(fds[0]);
            _exit(run_job(fds[1]));
        }
        close(fds[1]);
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        m_worker = pid;
        m_pipe = fds[0];
        m_received.clear();
        m_watch = m_loop.watch(m_pipe, Core::EventLoop::Interest::Read, [this] { read_from_worker(); });
    }

    // In the worker. A line for each thing to say: "P percent", and last "S" or "F message".
    int run_job(int out)
    {
        ErrorOr<void> result = Error::from_string_literal("nothing done");
        switch (m_current.kind) {
        case JobKind::Refresh:
            result = Package::refresh();
            break;
        case JobKind::Install:
            result = Package::install(m_current.name.view(), [out](u32 percent) { dprintf(out, "P %u\n", percent); });
            break;
        case JobKind::Remove:
            result = Package::remove(m_current.name.view());
            break;
        }
        if (result.is_error()) {
            if (result.error().is_errno())
                dprintf(out, "F %s\n", strerror(result.error().code()));
            else
                dprintf(out, "F %.*s\n", static_cast<int>(result.error().string_literal().length()), result.error().string_literal().characters_without_null_termination());
            return 1;
        }
        dprintf(out, "S\n");
        return 0;
    }

    void read_from_worker()
    {
        char chunk[512];
        ssize_t got = read(m_pipe, chunk, sizeof(chunk));
        if (got > 0) {
            for (ssize_t i = 0; i < got; ++i) {
                if (chunk[i] != '\n') {
                    m_received.append(chunk[i]);
                    continue;
                }
                process_line({ m_received.data(), m_received.size() });
                m_received.clear();
            }
            return;
        }
        if (got < 0 && errno == EINTR)
            return;
        m_loop.unwatch(m_watch);
        close(m_pipe);
        m_pipe = -1;
        int status = 0;
        waitpid(m_worker, &status, 0);
        m_worker = -1;
        if (!m_finished_said)
            finished_job(false, "The installer ran into trouble."_sv);
        m_finished_said = false;
        run_next();
    }

    void process_line(StringView line)
    {
        if (line.is_empty())
            return;
        if (line[0] == 'P' && line.length() > 2) {
            u32 percent = 0;
            for (size_t i = 2; i < line.length(); ++i)
                percent = percent * 10 + static_cast<u32>(line[i] - '0');
            tell([&](InstallerEventsProxy& listener) { return listener.progress(m_current.name.view(), percent); });
        } else if (line[0] == 'S') {
            m_finished_said = true;
            finished_job(true, {});
        } else if (line[0] == 'F') {
            m_finished_said = true;
            finished_job(false, line.length() > 2 ? line.substring_view(2) : StringView {});
        }
    }

    void finished_job(bool succeeded, StringView message)
    {
        os_log(m_log, "%s %s: %s", m_current.kind == JobKind::Refresh ? "refresh" : (m_current.kind == JobKind::Install ? "install" : "remove"), m_current.name.characters(), succeeded ? "done" : "failed");
        tell([&](InstallerEventsProxy& listener) { return listener.finished(m_current.name.view(), succeeded, message); });
        tell([](InstallerEventsProxy& listener) { return listener.changed(); });
    }

    Core::EventLoop& m_loop;
    os_log_t m_log;
    Vector<NonnullOwnPtr<InstallerEventsProxy>> m_listeners;
    Vector<Job> m_jobs;
    Job m_current { JobKind::Refresh, {} };
    pid_t m_worker { -1 };
    int m_pipe { -1 };
    int m_watch { -1 };
    bool m_finished_said { false };
    Vector<char> m_received;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    Package::trust_installed_code();
    auto port = IPC::check_in("com.vulture.installd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "installd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    Installer installer(loop);
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)installer.handle(message); });
    return loop.exec();
}
