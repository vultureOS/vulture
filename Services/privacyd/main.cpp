/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/privacy.h>
#include <LibBase/OwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <PrivacyEndpoint.h>
#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

// com.vulture.privacyd: the kernel asks whether an application may have something a person keeps
// private (see Kernel/Security/Privacy.h). If they have said, that's the answer. If not, and there is
// someone logged in to ask, their shell asks them.
//
// What they said is in a file only root can read, so that an application can't write its own "yes";
// and nothing installed can give an answer here either: that's what the kernel's word on the sender
// is for.

namespace {

constexpr char const* decisions_path = "/Library/Preferences/com.vulture.privacy.json";
constexpr char const* decisions_path_new = "/Library/Preferences/com.vulture.privacy.json.new";

struct Decision {
    String application;
    u32 uid;
    u32 service;
    bool allowed;
};

struct Pending {
    u32 id;
    u32 uid;
    u32 service;
    String application;
};

class Service final : public PrivacyStub {
public:
    Service(Core::EventLoop& loop)
        : m_log(os_log_create("com.vulture.privacyd", "decisions"))
    {
        load();
        m_device = open("/dev/privacy", O_RDWR | O_CLOEXEC);
        if (m_device < 0) {
            os_log_error(m_log, "/dev/privacy: %s", strerror(errno));
            return;
        }
        loop.watch(m_device, Core::EventLoop::Interest::Read, [this] { read_question(); });
    }

    void subscribe(IPC::Sender const& sender, IPC::Port prompts) override
    {
        if (is_installed_code(sender.pid)) {
            os_log(m_log, "Process %d isn't something that came with the system: it doesn't ask people anything", static_cast<int>(sender.pid));
            return;
        }
        // The newest: a session that went left its old one behind.
        m_prompts = adopt_own(*new PrivacyPromptsProxy(move(prompts)));
        m_prompter_uid = sender.uid;
    }

    void answer(IPC::Sender const& sender, u32 id, bool allowed) override
    {
        for (size_t i = 0; i < m_pending.size(); ++i) {
            if (m_pending[i].id != id)
                continue;
            if (is_installed_code(sender.pid) || (sender.uid != 0 && sender.uid != m_pending[i].uid)) {
                os_log(m_log, "Process %d tried to answer for someone else", static_cast<int>(sender.pid));
                return;
            }
            auto const pending = move(m_pending[i]);
            m_pending.remove(i);
            remember(pending.application.view(), pending.uid, pending.service, allowed);
            tell_kernel(pending.id, allowed);
            return;
        }
    }

    String decisions(IPC::Sender const& sender) override
    {
        auto list = JSON::Value::array();
        for (auto const& decision : m_decisions) {
            if (decision.uid != sender.uid && sender.uid != 0)
                continue;
            auto entry = JSON::Value::object();
            MUST(entry.set("application"_sv, MUST(JSON::Value::string(decision.application.view()))));
            MUST(entry.set("service"_sv, JSON::Value(static_cast<i64>(decision.service))));
            MUST(entry.set("allowed"_sv, JSON::Value(decision.allowed)));
            MUST(list.append(move(entry)));
        }
        return MUST(list.serialize());
    }

    void set_decision(IPC::Sender const& sender, String application, u32 service, bool allowed) override
    {
        if (is_installed_code(sender.pid)) {
            os_log(m_log, "Process %d tried to change what %s may have", static_cast<int>(sender.pid), application.characters());
            return;
        }
        remember(application.view(), sender.uid, service, allowed);
    }

private:
    [[nodiscard]] bool is_installed_code(pid_t pid)
    {
        // If the kernel can't say, it isn't anyone to take an answer from.
        i32 question = pid;
        int answer = 1;
        size_t size = sizeof(answer);
        if (sysctlbyname("security.process.third-party", &answer, &size, &question, sizeof(question)) != 0)
            return true;
        return answer != 0;
    }

    // One at a time: the device blocks when there are none, and the loop comes back while there are.
    void read_question()
    {
        vulture_privacy_request request;
        ssize_t got = read(m_device, &request, sizeof(request));
        if (got != sizeof(request))
            return;
        request.application[sizeof(request.application) - 1] = '\0';
        StringView const application { request.application, strlen(request.application) };
        if (auto const* known = find(application, request.uid, request.service)) {
            tell_kernel(request.id, known->allowed);
            return;
        }
        // Nobody to ask: no, and no decision kept, as nobody made one.
        if (!m_prompts || (m_prompter_uid != request.uid && m_prompter_uid != 0)) {
            os_log(m_log, "%s asked for service %u with nobody to ask", request.application, request.service);
            tell_kernel(request.id, false);
            return;
        }
        if (m_prompts->ask(request.id, application, request.service).is_error()) {
            m_prompts = nullptr;
            tell_kernel(request.id, false);
            return;
        }
        m_pending.append(Pending { request.id, request.uid, request.service, MUST(String::from_view(application)) });
    }

    void tell_kernel(u32 id, bool allowed)
    {
        vulture_privacy_answer answer { id, allowed ? 1u : 0u };
        (void)!write(m_device, &answer, sizeof(answer));
    }

    Decision* find(StringView application, u32 uid, u32 service)
    {
        for (auto& decision : m_decisions) {
            if (decision.uid == uid && decision.service == service && decision.application.view() == application)
                return &decision;
        }
        return nullptr;
    }

    void remember(StringView application, u32 uid, u32 service, bool allowed)
    {
        os_log(m_log, "%.*s %s service %u", static_cast<int>(application.length()), application.characters_without_null_termination(), allowed ? "may have" : "may not have", service);
        if (auto* known = find(application, uid, service))
            known->allowed = allowed;
        else
            m_decisions.append(Decision { MUST(String::from_view(application)), uid, service, allowed });
        save();
    }

    void load()
    {
        int fd = open(decisions_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return;
        static char text[64 * 1024];
        ssize_t got = read(fd, text, sizeof(text));
        close(fd);
        if (got <= 0)
            return;
        auto parsed = JSON::parse({ text, static_cast<size_t>(got) });
        if (parsed.is_error() || !parsed.value().is_array())
            return;
        for (size_t i = 0; i < parsed.value().size(); ++i) {
            auto const& entry = parsed.value().at(i);
            auto const application = entry.get_string("application"_sv);
            auto const uid = entry.get_integer("uid"_sv);
            auto const service = entry.get_integer("service"_sv);
            auto const allowed = entry.get_boolean("allowed"_sv);
            if (application.has_value() && uid.has_value() && service.has_value() && allowed.has_value())
                m_decisions.append(Decision { MUST(String::from_view(application.value())), static_cast<u32>(uid.value()), static_cast<u32>(service.value()), allowed.value() });
        }
    }

    void save()
    {
        auto list = JSON::Value::array();
        for (auto const& decision : m_decisions) {
            auto entry = JSON::Value::object();
            MUST(entry.set("application"_sv, MUST(JSON::Value::string(decision.application.view()))));
            MUST(entry.set("uid"_sv, JSON::Value(static_cast<i64>(decision.uid))));
            MUST(entry.set("service"_sv, JSON::Value(static_cast<i64>(decision.service))));
            MUST(entry.set("allowed"_sv, JSON::Value(decision.allowed)));
            MUST(list.append(move(entry)));
        }
        auto const text = MUST(list.serialize(true));
        int fd = open(decisions_path_new, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            os_log_error(m_log, "%s: %s", decisions_path_new, strerror(errno));
            return;
        }
        bool const written = write(fd, text.characters(), text.length()) == static_cast<ssize_t>(text.length());
        fsync(fd);
        close(fd);
        if (!written || rename(decisions_path_new, decisions_path) != 0) {
            os_log_error(m_log, "%s: %s", decisions_path, strerror(errno));
            unlink(decisions_path_new);
        }
    }

    os_log_t m_log;
    int m_device { -1 };
    OwnPtr<PrivacyPromptsProxy> m_prompts;
    u32 m_prompter_uid { 0 };
    Vector<Decision> m_decisions;
    Vector<Pending> m_pending;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.privacyd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "privacyd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    Service service(loop);
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
