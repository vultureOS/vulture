/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/sysctl.h>
#include <LibBase/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LogEndpoint.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

// logd: the unified log (com.vulture.logd). Programs send entries with
// os_log (Log.ipc); logd keeps the most recent in memory for `log show`
// and passes each on to the `log stream`s watching.

namespace {

constexpr size_t kept_entries = 4096;

struct Entry {
    u64 time { 0 };
    u64 thread { 0 };
    pid_t pid { 0 };
    u8 type { 0 };
    String process;
    String subsystem;
    String category;
    String message;
};

// A filter, as `log` asks for one.
struct Filter {
    u8 level { 0 };
    pid_t pid { 0 };
    String process;

    [[nodiscard]] bool matches(Entry const& entry) const
    {
        if (entry.type == 1 && level < 1)
            return false;
        if (entry.type == 2 && level < 2)
            return false;
        if (pid && entry.pid != pid)
            return false;
        if (!process.is_empty() && entry.process != process)
            return false;
        return true;
    }
};

StringView type_name(u8 type)
{
    switch (type) {
    case 1:
        return "Info"_sv;
    case 2:
        return "Debug"_sv;
    case 16:
        return "Error"_sv;
    case 17:
        return "Fault"_sv;
    default:
        return "Default"_sv;
    }
}

// The counts `log show` sums up: default, info, debug, error, fault.
size_t type_index(u8 type)
{
    switch (type) {
    case 1:
        return 1;
    case 2:
        return 2;
    case 16:
        return 3;
    case 17:
        return 4;
    default:
        return 0;
    }
}

// As macOS's log prints an entry (the default style):
// 2026-11-01 09:41:00.123456+0000 0x2a       Default     0x0                  57     0    notifyd: [com.vulture.notifyd:server] message
String format(Entry const& entry)
{
    time_t seconds = static_cast<time_t>(entry.time / 1'000'000);
    struct tm broken {};
    localtime_r(&seconds, &broken);
    char date[32], zone[8], timestamp[48];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &broken);
    strftime(zone, sizeof(zone), "%z", &broken);
    snprintf(timestamp, sizeof(timestamp), "%s.%06u%s", date, static_cast<unsigned>(entry.time % 1'000'000), zone);
    char thread[24];
    snprintf(thread, sizeof(thread), "0x%llx", static_cast<unsigned long long>(entry.thread));

    StringBuilder line;
    char columns[160];
    snprintf(columns, sizeof(columns), "%-31s %-10s %-11s %-20s %-6d %-4d ", timestamp, thread, type_name(entry.type).characters_without_null_termination(),
        "0x0", entry.pid, 0);
    // type_name()'s views are literals: terminated.
    line.append(StringView(columns));
    line.append(entry.process.view());
    line.append(": "_sv);
    if (!entry.subsystem.is_empty()) {
        line.append('[');
        line.append(entry.subsystem.view());
        line.append(':');
        line.append(entry.category.view());
        line.append("] "_sv);
    }
    line.append(entry.message.view());
    return MUST(line.to_string());
}

// Process names by pid, from the kernel's process table.
class ProcessNames {
public:
    // The name of `pid` (refreshing the table for a pid not seen yet), or
    // nothing if it has gone.
    Optional<String> name(pid_t pid)
    {
        if (auto const* known = m_names.get(pid))
            return *known;
        refresh();
        if (auto const* known = m_names.get(pid))
            return *known;
        return {};
    }

private:
    void refresh()
    {
        size_t length = 0;
        if (sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0) < 0)
            return;
        length += 16 * sizeof(vulture_process_info);
        auto* processes = static_cast<vulture_process_info*>(malloc(length));
        if (!processes)
            return;
        if (sysctlbyname("kern.proc.all", processes, &length, nullptr, 0) == 0) {
            // A fresh table: names of processes gone are forgotten (their
            // pids may come back as something else).
            m_names.clear();
            size_t count = length / sizeof(vulture_process_info);
            for (size_t i = 0; i < count; ++i) {
                auto& process = processes[i];
                process.name[sizeof(process.name) - 1] = '\0';
                m_names.set(process.pid, MUST(String::from_view(StringView(process.name))));
            }
        }
        free(processes);
    }

    HashMap<pid_t, String> m_names;
};

class LogService final : public LogStub {
public:
    LogService()
    {
        (void)m_entries.try_ensure_capacity(kept_entries);
        add(getpid(), 0, static_cast<u64>(gettid()), "logd"_sv, "com.vulture.logd"_sv, "server"_sv, "Starting"_sv);
    }

    void log(IPC::Sender const& sender, u64 time, u64 thread, String process, u8 type, String subsystem, String category, String message) override
    {
        auto name = m_names.name(sender.pid);
        add(sender.pid, time, thread, name.has_value() ? name->view() : process.view(), subsystem.view(), category.view(), message.view(), type);
    }

    LogEndpoint::ShowAnswer show(IPC::Sender const&, u64 since, u8 level, i32 pid, String process) override
    {
        Filter filter { level, pid, move(process) };
        LogEndpoint::ShowAnswer answer;
        for (size_t i = 0; i < 5; ++i)
            answer.counts.append(0);
        // An answer must fit in a message: the most recent entries that
        // match, as many as fit, oldest first.
        constexpr size_t room = 60 * 1024;
        size_t bytes = 0;
        size_t first = m_entries.size();
        while (first > 0) {
            auto const& entry = at(first - 1);
            if (entry.time < since)
                break;
            if (filter.matches(entry)) {
                size_t length = format(entry).length() + 4;
                if (bytes + length > room)
                    break;
                bytes += length;
            }
            --first;
        }
        for (size_t i = first; i < m_entries.size(); ++i) {
            auto const& entry = at(i);
            if (!filter.matches(entry))
                continue;
            ++answer.counts[type_index(entry.type)];
            answer.lines.append(format(entry));
        }
        return answer;
    }

    bool stream(IPC::Sender const&, IPC::Port sink, u8 level, i32 pid, String process) override
    {
        if (!sink.is_valid())
            return false;
        m_sinks.append({ move(sink), Filter { level, pid, move(process) } });
        return true;
    }

private:
    struct Sink {
        IPC::Port port;
        Filter filter;
    };

    Entry const& at(size_t index) const { return m_entries[(m_first + index) % m_entries.size()]; }

    void add(pid_t pid, u64 time, u64 thread, StringView process, StringView subsystem, StringView category, StringView message, u8 type = 0)
    {
        Entry entry;
        entry.time = time;
        entry.thread = thread;
        entry.pid = pid;
        entry.type = type;
        entry.process = MUST(String::from_view(process));
        entry.subsystem = MUST(String::from_view(subsystem));
        entry.category = MUST(String::from_view(category));
        entry.message = MUST(String::from_view(message));
        if (entry.time == 0) {
            timespec now {};
            clock_gettime(CLOCK_REALTIME, &now);
            entry.time = static_cast<u64>(now.tv_sec) * 1'000'000 + static_cast<u64>(now.tv_nsec) / 1000;
        }
        publish(entry);
        show_on_console(entry);
        if (m_entries.size() < kept_entries) {
            m_entries.append(move(entry));
        } else {
            m_entries[m_first] = move(entry);
            m_first = (m_first + 1) % kept_entries;
        }
    }

    // Booted with "debug": everything, at every level, as it arrives, shorter than
    // `log` prints it (the same entries are in `log show`).
    void show_on_console(Entry const& entry)
    {
        if (!getenv("VULTURE_DEBUG"))
            return;
        static int const console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (console < 0)
            return;
        auto seconds = static_cast<time_t>(entry.time / 1'000'000);
        struct tm broken {};
        gmtime_r(&seconds, &broken);
        StringBuilder line;
        line.appendff("log: {:02}:{:02}:{:02}.{:03} ", broken.tm_hour, broken.tm_min, broken.tm_sec, (entry.time % 1'000'000) / 1000);
        line.append(type_name(entry.type));
        line.appendff(" {}[{}]", entry.process.view(), entry.pid);
        if (!entry.subsystem.is_empty()) {
            line.append(" ["_sv);
            line.append(entry.subsystem.view());
            line.append(":"_sv);
            line.append(entry.category.view());
            line.append("]"_sv);
        }
        line.append(" "_sv);
        line.append(entry.message.view());
        line.append("\n"_sv);
        auto text = line.view();
        (void)write(console, text.characters_without_null_termination(), text.length());
    }

    // To every stream that wants it. A stream that has gone (or cannot
    // keep up: logd never waits for one) is dropped.
    void publish(Entry const& entry)
    {
        Optional<String> line;
        for (size_t i = 0; i < m_sinks.size();) {
            auto& sink = m_sinks[i];
            if (!sink.filter.matches(entry)) {
                ++i;
                continue;
            }
            if (!line.has_value())
                line = format(entry);
            IPC::Encoder encoder(LogStreamEndpoint::magic, static_cast<u32>(LogStreamEndpoint::Method::Entry));
            encoder.encode(line->view());
            auto sent = IPC::send(sink.port, encoder, true);
            if (sent.is_error() && sent.error().code() != EAGAIN) {
                m_sinks.remove(i);
                continue;
            }
            ++i;
        }
    }

    Vector<Entry> m_entries; // A ring once full; m_first is the oldest.
    size_t m_first { 0 };
    Vector<Sink> m_sinks;
    ProcessNames m_names;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto port = IPC::check_in("com.vulture.logd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "logd: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }
    Core::EventLoop loop;
    LogService service;
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); });
    return loop.exec();
}
