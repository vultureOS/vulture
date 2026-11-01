/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ConfigurationEndpoint.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// configd: the system configuration (com.vulture.SystemConfiguration.configd).
// It keeps the machine's names, as macOS does, in
// /Library/Preferences/SystemConfiguration/preferences.plist:
//
//   ComputerName    what people call the machine ("Krisna's Mac")
//   LocalHostName   its name on the local network ("Krisnas-Mac")
//   HostName        its DNS name, if it has one
//
// and makes the kernel's host name HostName, or LocalHostName.local
// without one. A change is posted as com.vulture.system.hostname.
//
//   configd [--preferences FILE] [--keep-hostname]

namespace {

constexpr StringView default_preferences = "/Library/Preferences/SystemConfiguration/preferences.plist"_sv;

bool is_hostname_label_character(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

// A DNS label: letters, digits and hyphens, not at either end.
bool is_label(StringView label)
{
    if (label.is_empty() || label.length() > 63 || label[0] == '-' || label[label.length() - 1] == '-')
        return false;
    for (size_t i = 0; i < label.length(); ++i) {
        if (!is_hostname_label_character(label[i]))
            return false;
    }
    return true;
}

bool is_valid(StringView key, StringView value)
{
    if (key == "LocalHostName"_sv)
        return is_label(value);
    if (key == "HostName"_sv) {
        if (value.is_empty() || value.length() > 253)
            return false;
        size_t start = 0;
        for (size_t i = 0; i <= value.length(); ++i) {
            if (i == value.length() || value[i] == '.') {
                if (!is_label(value.substring_view(start, i - start)))
                    return false;
                start = i + 1;
            }
        }
        return true;
    }
    if (key == "ComputerName"_sv) {
        if (value.is_empty() || value.length() > 63)
            return false;
        for (size_t i = 0; i < value.length(); ++i) {
            if (static_cast<unsigned char>(value[i]) < 0x20 || value[i] == 0x7f)
                return false;
        }
        return true;
    }
    return false;
}

class ConfigurationService final : public ConfigurationStub {
public:
    ConfigurationService(StringView preferences, bool apply_hostname)
        : m_preferences(MUST(String::from_view(preferences)))
        , m_apply_hostname(apply_hostname)
        , m_log(os_log_create("com.vulture.SystemConfiguration", "configd"))
    {
        load();
        // The names a machine always has (macOS's come from the first
        // setup; ours from the name it booted with).
        char host[256] = "vulture";
        gethostname(host, sizeof(host));
        if (char* dot = strchr(host, '.'))
            *dot = '\0';
        bool seeded = false;
        if (m_local_host_name.is_empty() && is_label(StringView(host))) {
            m_local_host_name = MUST(String::from_view(StringView(host)));
            seeded = true;
        }
        if (m_computer_name.is_empty()) {
            m_computer_name = MUST(String::from_view(m_local_host_name.is_empty() ? "vultureOS"_sv : m_local_host_name.view()));
            seeded = true;
        }
        if (seeded)
            (void)save();
        os_log(m_log, "Starting: ComputerName \"%s\", LocalHostName \"%s\", HostName %s", m_computer_name.characters(),
            m_local_host_name.characters(), m_host_name.is_empty() ? "not set" : m_host_name.characters());
        apply();
    }

    ConfigurationEndpoint::GetAnswer get(IPC::Sender const&, String key) override
    {
        ConfigurationEndpoint::GetAnswer answer;
        if (auto* value = slot(key.view()); value && !value->is_empty()) {
            answer.found = true;
            answer.value = *value;
        }
        return answer;
    }

    i32 set(IPC::Sender const& sender, String key, String value) override
    {
        auto* target = slot(key.view());
        if (!target)
            return EINVAL;
        if (sender.uid != 0) {
            os_log_error(m_log, "%d (uid %u) may not set %s", sender.pid, sender.uid, key.characters());
            return EPERM;
        }
        if (!is_valid(key.view(), value.view()))
            return EINVAL;
        if (*target == value)
            return 0;
        auto previous = move(*target);
        *target = move(value);
        if (auto saved = save(); saved.is_error()) {
            *target = move(previous);
            os_log_error(m_log, "Could not save %s: %s", m_preferences.characters(), strerror(saved.error().code()));
            return saved.error().code();
        }
        os_log(m_log, "%s set to \"%s\" by %d", key.characters(), target->characters(), sender.pid);
        apply();
        return 0;
    }

    i32 remove(IPC::Sender const& sender, String key) override
    {
        auto* target = slot(key.view());
        if (!target)
            return EINVAL;
        if (sender.uid != 0)
            return EPERM;
        // A machine always has its ComputerName and LocalHostName.
        if (key.view() != "HostName"_sv)
            return EINVAL;
        if (target->is_empty())
            return 0;
        auto previous = move(*target);
        *target = String();
        if (auto saved = save(); saved.is_error()) {
            *target = move(previous);
            return saved.error().code();
        }
        os_log(m_log, "%s removed by %d", key.characters(), sender.pid);
        apply();
        return 0;
    }

    Vector<String> keys(IPC::Sender const&) override
    {
        Vector<String> keys;
        StringView const names[] = { "ComputerName"_sv, "HostName"_sv, "LocalHostName"_sv };
        for (auto name : names) {
            if (auto* value = slot(name); value && !value->is_empty())
                keys.append(MUST(String::from_view(name)));
        }
        return keys;
    }

private:
    String* slot(StringView key)
    {
        if (key == "ComputerName"_sv)
            return &m_computer_name;
        if (key == "LocalHostName"_sv)
            return &m_local_host_name;
        if (key == "HostName"_sv)
            return &m_host_name;
        return nullptr;
    }

    // { "System" : { "System" : { "ComputerName", "HostName" },
    //                "Network" : { "HostNames" : { "LocalHostName" } } } }
    void load()
    {
        int fd = open(m_preferences.characters(), O_RDONLY);
        if (fd < 0)
            return;
        StringBuilder text;
        char buffer[1024];
        ssize_t got;
        while ((got = read(fd, buffer, sizeof(buffer))) > 0)
            text.append(StringView(buffer, static_cast<size_t>(got)));
        close(fd);
        auto root = JSON::parse(text.view());
        if (root.is_error() || !root.value().is_object()) {
            os_log_error(m_log, "%s is not a preferences file; starting afresh", m_preferences.characters());
            return;
        }
        auto const* outer = root.value().get_object("System"_sv);
        if (!outer)
            return;
        if (auto const* system = outer->get_object("System"_sv)) {
            if (auto name = system->get_string("ComputerName"_sv); name.has_value() && is_valid("ComputerName"_sv, name.value()))
                m_computer_name = MUST(String::from_view(name.value()));
            if (auto name = system->get_string("HostName"_sv); name.has_value() && is_valid("HostName"_sv, name.value()))
                m_host_name = MUST(String::from_view(name.value()));
        }
        if (auto const* network = outer->get_object("Network"_sv)) {
            if (auto const* names = network->get_object("HostNames"_sv)) {
                if (auto name = names->get_string("LocalHostName"_sv); name.has_value() && is_valid("LocalHostName"_sv, name.value()))
                    m_local_host_name = MUST(String::from_view(name.value()));
            }
        }
    }

    // Written whole to a new file that then takes the old one's place, so
    // a crash leaves one or the other.
    ErrorOr<void> save()
    {
        auto system = JSON::Value::object();
        TRY(system.set("ComputerName"_sv, TRY(JSON::Value::string(m_computer_name.view()))));
        if (!m_host_name.is_empty())
            TRY(system.set("HostName"_sv, TRY(JSON::Value::string(m_host_name.view()))));
        auto host_names = JSON::Value::object();
        TRY(host_names.set("LocalHostName"_sv, TRY(JSON::Value::string(m_local_host_name.view()))));
        auto network = JSON::Value::object();
        TRY(network.set("HostNames"_sv, move(host_names)));
        auto outer = JSON::Value::object();
        TRY(outer.set("System"_sv, move(system)));
        TRY(outer.set("Network"_sv, move(network)));
        auto root = JSON::Value::object();
        TRY(root.set("System"_sv, move(outer)));
        auto text = TRY(root.serialize(true));

        // The directory, as macOS has it (/Library/Preferences is there).
        auto directory = m_preferences.view();
        if (auto slash = directory.find_last('/'); slash.has_value() && slash.value() > 0) {
            auto path = TRY(String::from_view(directory.substring_view(0, slash.value())));
            if (mkdir(path.characters(), 0755) < 0 && errno != EEXIST)
                return Error::from_errno(errno);
        }
        StringBuilder temporary;
        temporary.append(m_preferences.view());
        temporary.append(".new"_sv);
        auto temporary_path = TRY(temporary.to_string());
        int fd = open(temporary_path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            return Error::from_errno(errno);
        bool written = write(fd, text.characters(), text.length()) == static_cast<ssize_t>(text.length()) && write(fd, "\n", 1) == 1;
        int error = errno;
        if (written && fsync(fd) < 0) {
            written = false;
            error = errno;
        }
        close(fd);
        if (!written || rename(temporary_path.characters(), m_preferences.characters()) < 0) {
            if (written)
                error = errno;
            unlink(temporary_path.characters());
            return Error::from_errno(error);
        }
        return {};
    }

    // The kernel's host name, from the names.
    void apply()
    {
        if (!m_apply_hostname)
            return;
        StringBuilder name;
        if (!m_host_name.is_empty()) {
            name.append(m_host_name.view());
        } else if (!m_local_host_name.is_empty()) {
            name.append(m_local_host_name.view());
            name.append(".local"_sv);
        } else {
            return;
        }
        char current[256] = {};
        gethostname(current, sizeof(current));
        if (name.view() == StringView(current))
            return;
        if (sethostname(name.view().characters_without_null_termination(), name.length()) < 0) {
            os_log_error(m_log, "Could not set the host name: %s", strerror(errno));
            return;
        }
        notify_post("com.vulture.system.hostname");
    }

    String m_preferences;
    bool m_apply_hostname { true };
    os_log_t m_log;
    String m_computer_name;
    String m_local_host_name;
    String m_host_name;
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    StringView preferences = default_preferences;
    bool apply_hostname = true;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--preferences") == 0 && i + 1 < argc) {
            preferences = StringView(argv[++i]);
        } else if (strcmp(argv[i], "--keep-hostname") == 0) {
            apply_hostname = false;
        } else {
            fprintf(stderr, "usage: configd [--preferences FILE] [--keep-hostname]\n");
            return 64;
        }
    }
    auto port = IPC::check_in("com.vulture.SystemConfiguration.configd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "configd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    ConfigurationService service(preferences, apply_hostname);
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
