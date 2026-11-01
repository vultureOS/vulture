/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <LogEndpoint.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// log: the unified log, as macOS's log shows it.
//   log stream [--level default|info|debug] [--process pid|name] [--timeout n[m|h|d]]
//   log show [--last n[m|h|d] | --last boot] [--info] [--debug] [--process pid|name]

namespace {

void usage(FILE* stream)
{
    fprintf(stream,
        "usage:\n"
        "    log <command>\n"
        "\n"
        "global options:\n"
        "    -?, --help\n"
        "\n"
        "commands:\n"
        "    show            view/search system logs\n"
        "    stream          watch live system logs\n"
        "\n"
        "further help:\n"
        "    log help <command>\n");
}

void usage_for(char const* command)
{
    if (strcmp(command, "stream") == 0) {
        printf("usage:\n"
               "    log stream [options]\n"
               "\n"
               "description:\n"
               "    Stream activities, log data or trace messages in real time.\n"
               "\n"
               "options:\n"
               "    --level default | info | debug   Include events at, and below, the given level\n"
               "    --process <pid> | <process>      Filter by process\n"
               "    --timeout <num>[m|h|d]           Timeout log stream after the given interval\n");
    } else if (strcmp(command, "show") == 0) {
        printf("usage:\n"
               "    log show [options]\n"
               "\n"
               "description:\n"
               "    Shows contents of the system log.\n"
               "\n"
               "options:\n"
               "    --debug                          Show debug messages\n"
               "    --info                           Show info messages\n"
               "    --last <num>[m|h|d] | boot       Limits the shown events to the given interval\n"
               "    --process <pid> | <process>      Filter by process\n");
    } else {
        usage(stdout);
    }
}

// "30", "30s", "5m", "2h", "1d": seconds; -1 if it is none of those.
long long interval_seconds(char const* text)
{
    char* end = nullptr;
    long long value = strtoll(text, &end, 10);
    if (end == text || value < 0)
        return -1;
    switch (*end) {
    case '\0':
    case 's':
        break;
    case 'm':
        value *= 60;
        break;
    case 'h':
        value *= 3600;
        break;
    case 'd':
        value *= 86400;
        break;
    default:
        return -1;
    }
    if (*end && end[1])
        return -1;
    return value;
}

u64 now_microseconds()
{
    timeval now {};
    gettimeofday(&now, nullptr);
    return static_cast<u64>(now.tv_sec) * 1'000'000 + static_cast<u64>(now.tv_usec);
}

void print_header()
{
    printf("%-31s %-10s %-11s %-20s %-6s %-4s \n", "Timestamp", "Thread", "Type", "Activity", "PID", "TTL");
}

struct Options {
    u8 level { 0 };
    i32 pid { 0 };
    char const* process { "" };
    long long timeout { -1 };
    long long last { -1 };
    bool since_boot { false };
};

bool parse_process(char const* text, Options& options)
{
    char* end = nullptr;
    long pid = strtol(text, &end, 10);
    if (end != text && *end == '\0' && pid > 0)
        options.pid = static_cast<i32>(pid);
    else
        options.process = text;
    return true;
}

LogProxy* connect()
{
    auto port = IPC::look_up("com.vulture.logd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "log: Could not connect to logd: %s\n", strerror(port.error().code()));
        return nullptr;
    }
    return new LogProxy(port.release_value());
}

class Printer final : public LogStreamStub {
public:
    void entry(IPC::Sender const&, String line) override
    {
        fwrite(line.characters(), 1, line.length(), stdout);
        fputc('\n', stdout);
    }
};

int stream(Options const& options)
{
    auto* logd = connect();
    if (!logd)
        return 1;
    auto port = IPC::create_port();
    if (port.is_error()) {
        fprintf(stderr, "log: %s\n", strerror(port.error().code()));
        return 1;
    }
    auto accepted = logd->stream(move(port.value().send), options.level, options.pid, StringView(options.process));
    if (accepted.is_error() || !accepted.value()) {
        fprintf(stderr, "log: Could not stream from logd: %s\n", accepted.is_error() ? strerror(accepted.error().code()) : "refused");
        return 1;
    }
    if (options.pid)
        printf("Filtering the log data using \"processIdentifier == %d\"\n", options.pid);
    else if (*options.process)
        printf("Filtering the log data using \"process == \"%s\"\"\n", options.process);
    print_header();
    fflush(stdout);

    Printer printer;
    u64 deadline = options.timeout >= 0 ? now_microseconds() + static_cast<u64>(options.timeout) * 1'000'000 : 0;
    for (;;) {
        int wait = -1;
        if (deadline) {
            u64 now = now_microseconds();
            if (now >= deadline)
                return 0;
            wait = static_cast<int>((deadline - now + 999) / 1000);
        }
        pollfd watched { port.value().receive.fd(), POLLIN, 0 };
        int ready = poll(&watched, 1, wait);
        if (ready < 0 && errno != EINTR)
            return 1;
        if (ready <= 0)
            continue;
        // Everything waiting, then the output.
        for (;;) {
            auto message = IPC::receive(port.value().receive, true);
            if (message.is_error()) {
                if (message.error().code() == EAGAIN)
                    break;
                // logd went away.
                fprintf(stderr, "log: logd stopped streaming\n");
                return 1;
            }
            (void)printer.handle(message.value());
        }
        fflush(stdout);
    }
}

int show(Options const& options)
{
    auto* logd = connect();
    if (!logd)
        return 1;
    u64 since = 0;
    if (options.last >= 0) {
        u64 span = static_cast<u64>(options.last) * 1'000'000;
        u64 now = now_microseconds();
        since = span < now ? now - span : 0;
    } else if (options.since_boot) {
        timespec uptime {};
        clock_gettime(CLOCK_MONOTONIC, &uptime);
        u64 up = static_cast<u64>(uptime.tv_sec) * 1'000'000 + static_cast<u64>(uptime.tv_nsec) / 1000;
        u64 now = now_microseconds();
        since = up < now ? now - up : 0;
    }
    auto answer = logd->show(since, options.level, options.pid, StringView(options.process));
    if (answer.is_error()) {
        fprintf(stderr, "log: Could not read the log from logd: %s\n", strerror(answer.error().code()));
        return 1;
    }
    if (options.level == 0)
        printf("Skipping info and debug messages, pass --info and/or --debug to include.\n");
    else if (options.level == 1)
        printf("Skipping debug messages, pass --debug to include.\n");
    if (options.pid)
        printf("Filtering the log data using \"processIdentifier == %d\"\n", options.pid);
    else if (*options.process)
        printf("Filtering the log data using \"process == \"%s\"\"\n", options.process);
    print_header();
    for (auto const& line : answer.value().lines) {
        fwrite(line.characters(), 1, line.length(), stdout);
        fputc('\n', stdout);
    }
    auto const& counts = answer.value().counts;
    auto count = [&](size_t index) { return index < counts.size() ? counts[index] : 0u; };
    printf("--------------------------------------------------------------------------------------------------------------------\n");
    printf("Log      - Default: %10u, Info: %16u, Debug: %13u, Error: %10u, Fault: %10u\n", count(0), count(1), count(2), count(3), count(4));
    printf("Activity - Create: %11u, Transition: %10u, Actions: %11u\n", 0u, 0u, 0u);
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage(stderr);
        return 64;
    }
    char const* command = argv[1];
    if (strcmp(command, "-?") == 0 || strcmp(command, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(command, "help") == 0) {
        usage_for(argc > 2 ? argv[2] : "");
        return 0;
    }
    bool is_stream = strcmp(command, "stream") == 0;
    if (!is_stream && strcmp(command, "show") != 0) {
        fprintf(stderr, "log: unknown command \"%s\"\n", command);
        usage(stderr);
        return 64;
    }

    Options options;
    for (int i = 2; i < argc; ++i) {
        char const* option = argv[i];
        char const* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (is_stream && strcmp(option, "--level") == 0 && value) {
            if (strcmp(value, "default") == 0)
                options.level = 0;
            else if (strcmp(value, "info") == 0)
                options.level = 1;
            else if (strcmp(value, "debug") == 0)
                options.level = 2;
            else {
                fprintf(stderr, "log: invalid level \"%s\" (default, info or debug)\n", value);
                return 64;
            }
            ++i;
        } else if (strcmp(option, "--process") == 0 && value) {
            parse_process(value, options);
            ++i;
        } else if (is_stream && strcmp(option, "--timeout") == 0 && value) {
            options.timeout = interval_seconds(value);
            if (options.timeout < 0) {
                fprintf(stderr, "log: invalid timeout \"%s\"\n", value);
                return 64;
            }
            ++i;
        } else if (!is_stream && strcmp(option, "--last") == 0 && value) {
            if (strcmp(value, "boot") == 0) {
                options.since_boot = true;
            } else {
                options.last = interval_seconds(value);
                if (options.last < 0) {
                    fprintf(stderr, "log: invalid interval \"%s\"\n", value);
                    return 64;
                }
            }
            ++i;
        } else if (!is_stream && strcmp(option, "--info") == 0) {
            if (options.level < 1)
                options.level = 1;
        } else if (!is_stream && strcmp(option, "--debug") == 0) {
            options.level = 2;
        } else {
            fprintf(stderr, "log: unknown option \"%s\"\n", option);
            usage_for(command);
            return 64;
        }
    }
    int result = is_stream ? stream(options) : show(options);
    fflush(stdout);
    return result;
}
