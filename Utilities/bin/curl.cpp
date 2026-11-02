/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibHTTP/Client.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// curl [options] URL: the usual ones, with the usual meanings, messages and exit codes.

namespace {

struct Arguments {
    bool silent { false };
    bool show_errors { false };
    bool show_headers { false };
    bool head_only { false };
    bool verbose { false };
    bool fail_on_http_error { false };
    bool remote_name { false };
    char const* output_path { nullptr };
    char const* url { nullptr };
    char const* method { nullptr };
    char const* data { nullptr };
    char const* write_out { nullptr };
};

void usage()
{
    fprintf(stderr, "Usage: curl [options...] <url>\n"
                    " -A, --user-agent <name>    Send User-Agent <name>\n"
                    "     --cacert <file>        CA certificate to verify peer against\n"
                    "     --compressed           Request a compressed response\n"
                    "     --connect-timeout <s>  Maximum time allowed to connect\n"
                    " -d, --data <data>          HTTP POST data\n"
                    " -f, --fail                 Fail on HTTP errors, with no output\n"
                    " -H, --header <header>      Pass custom header to server\n"
                    " -I, --head                 Show headers only\n"
                    " -i, --include              Include response headers in the output\n"
                    " -k, --insecure             Allow servers whose certificate can't be checked\n"
                    " -L, --location             Follow redirects\n"
                    " -m, --max-time <seconds>   Maximum time allowed for the whole thing\n"
                    " -O, --remote-name          Write output to a file named as the remote one\n"
                    " -o, --output <file>        Write output to <file> instead of stdout\n"
                    " -S, --show-error           Show an error, even with -s\n"
                    " -s, --silent               Show nothing but the output\n"
                    " -v, --verbose              Say what is going on\n"
                    " -w, --write-out <format>   %%{http_code} or %%{size_download}, afterward\n"
                    " -X, --request <method>     Use this request method\n");
}

void format_size(u64 bytes, char* out, size_t size)
{
    if (bytes < 100000)
        snprintf(out, size, "%llu", static_cast<unsigned long long>(bytes));
    else if (bytes < 10000 * 1024ull)
        snprintf(out, size, "%lluk", static_cast<unsigned long long>(bytes / 1024));
    else if (bytes < 10000 * 1024ull * 1024)
        snprintf(out, size, "%lluM", static_cast<unsigned long long>(bytes / (1024 * 1024)));
    else
        snprintf(out, size, "%lluG", static_cast<unsigned long long>(bytes / (1024 * 1024 * 1024)));
}

double now_seconds()
{
    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

}

int main(int argc, char** argv)
{
    Arguments arguments;
    HTTP::Options options;
    Vector<HTTP::Header> headers;

    auto needs_value = [&](int& i) -> char const* {
        if (i + 1 >= argc) {
            fprintf(stderr, "curl: option %s: requires parameter\n", argv[i]);
            exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        char const* argument = argv[i];
        if (argument[0] != '-' || argument[1] == '\0') {
            arguments.url = argument;
            continue;
        }
        if (argument[1] == '-') {
            StringView const name { argument + 2, strlen(argument + 2) };
            if (name == "silent"_sv)
                arguments.silent = true;
            else if (name == "show-error"_sv)
                arguments.show_errors = true;
            else if (name == "include"_sv)
                arguments.show_headers = true;
            else if (name == "head"_sv)
                arguments.head_only = true;
            else if (name == "verbose"_sv)
                arguments.verbose = true;
            else if (name == "fail"_sv)
                arguments.fail_on_http_error = true;
            else if (name == "location"_sv)
                options.follow_redirects = true;
            else if (name == "insecure"_sv)
                options.insecure = true;
            else if (name == "compressed"_sv)
                options.accept_compression = true;
            else if (name == "remote-name"_sv)
                arguments.remote_name = true;
            else if (name == "output"_sv)
                arguments.output_path = needs_value(i);
            else if (name == "request"_sv)
                arguments.method = needs_value(i);
            else if (name == "data"_sv)
                arguments.data = needs_value(i);
            else if (name == "write-out"_sv)
                arguments.write_out = needs_value(i);
            else if (name == "header"_sv) {
                char const* value = needs_value(i);
                StringView const text { value, strlen(value) };
                auto const colon = text.find(':');
                if (colon.has_value()) {
                    HTTP::Header header;
                    header.name = MUST(String::from_view(text.substring_view(0, colon.value())));
                    StringView rest = text.substring_view(colon.value() + 1);
                    while (rest.starts_with(" "_sv))
                        rest = rest.substring_view(1);
                    header.value = MUST(String::from_view(rest));
                    headers.append(move(header));
                }
            } else if (name == "user-agent"_sv) {
                char const* value = needs_value(i);
                options.user_agent = { value, strlen(value) };
            } else if (name == "cacert"_sv) {
                char const* value = needs_value(i);
                options.certificates_file = { value, strlen(value) };
            } else if (name == "max-time"_sv)
                options.timeout_seconds = static_cast<u32>(atoi(needs_value(i)));
            else if (name == "connect-timeout"_sv)
                options.connect_timeout_seconds = static_cast<u32>(atoi(needs_value(i)));
            else if (name == "help"_sv) {
                usage();
                return 0;
            } else {
                fprintf(stderr, "curl: option %s: is unknown\n", argument);
                return 2;
            }
            continue;
        }
        for (char const* flag = argument + 1; *flag; ++flag) {
            switch (*flag) {
            case 's':
                arguments.silent = true;
                break;
            case 'S':
                arguments.show_errors = true;
                break;
            case 'i':
                arguments.show_headers = true;
                break;
            case 'I':
                arguments.head_only = true;
                break;
            case 'v':
                arguments.verbose = true;
                break;
            case 'f':
                arguments.fail_on_http_error = true;
                break;
            case 'L':
                options.follow_redirects = true;
                break;
            case 'k':
                options.insecure = true;
                break;
            case 'O':
                arguments.remote_name = true;
                break;
            case 'o':
                arguments.output_path = needs_value(i);
                break;
            case 'X':
                arguments.method = needs_value(i);
                break;
            case 'd':
                arguments.data = needs_value(i);
                break;
            case 'w':
                arguments.write_out = needs_value(i);
                break;
            case 'A': {
                char const* value = needs_value(i);
                options.user_agent = { value, strlen(value) };
                break;
            }
            case 'm':
                options.timeout_seconds = static_cast<u32>(atoi(needs_value(i)));
                break;
            case 'H': {
                char const* value = needs_value(i);
                StringView const text { value, strlen(value) };
                auto const colon = text.find(':');
                if (colon.has_value()) {
                    HTTP::Header header;
                    header.name = MUST(String::from_view(text.substring_view(0, colon.value())));
                    StringView rest = text.substring_view(colon.value() + 1);
                    while (rest.starts_with(" "_sv))
                        rest = rest.substring_view(1);
                    header.value = MUST(String::from_view(rest));
                    headers.append(move(header));
                }
                break;
            }
            default:
                fprintf(stderr, "curl: option -%c: is unknown\n", *flag);
                return 2;
            }
        }
    }
    if (!arguments.url) {
        fprintf(stderr, "curl: no URL specified\ncurl: try 'curl --help' for more information\n");
        return 2;
    }

    auto fail = [&](int code, StringView message) {
        if (!arguments.silent || arguments.show_errors)
            fprintf(stderr, "curl: (%d) %.*s\n", code, static_cast<int>(message.length()), message.characters_without_null_termination());
        return code;
    };

    auto url = HTTP::URL::parse({ arguments.url, strlen(arguments.url) });
    if (url.is_error()) {
        return fail(3, url.error().string_literal());
    }

    HTTP::Request request;
    request.url = url.release_value();
    request.headers = move(headers);
    if (arguments.head_only)
        request.method = "HEAD"_sv;
    if (arguments.data) {
        request.method = "POST"_sv;
        request.body = { reinterpret_cast<u8 const*>(arguments.data), strlen(arguments.data) };
        HTTP::Header type;
        type.name = MUST(String::from_view("Content-Type"_sv));
        type.value = MUST(String::from_view("application/x-www-form-urlencoded"_sv));
        request.headers.append(move(type));
    }
    if (arguments.method)
        request.method = { arguments.method, strlen(arguments.method) };

    int output_fd = 1;
    String remote_file;
    if (arguments.remote_name) {
        remote_file = request.url.file_name();
        if (remote_file.is_empty()) {
            return fail(23, "Remote file name has no length"_sv);
        }
        arguments.output_path = remote_file.characters();
    }
    bool const to_file = arguments.output_path && strcmp(arguments.output_path, "-") != 0;
    bool const show_progress = !arguments.silent && (to_file || !isatty(1)) && !arguments.verbose;

    bool file_opened = false;
    auto open_output = [&]() -> ErrorOr<void> {
        if (!to_file || file_opened)
            return {};
        output_fd = open(arguments.output_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (output_fd < 0)
            return Error::from_errno(errno);
        file_opened = true;
        return {};
    };
    auto write_output = [&](ReadonlyBytes bytes) -> ErrorOr<void> {
        size_t done = 0;
        while (done < bytes.size()) {
            ssize_t written = write(output_fd, bytes.data() + done, bytes.size() - done);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                return Error::from_errno(errno);
            }
            done += static_cast<size_t>(written);
        }
        return {};
    };

    double const started = now_seconds();
    double last_shown = -1;
    u64 last_total = 0;
    u64 last_received = 0;
    auto show_meter = [&](bool final_line) {
        double const elapsed = now_seconds() - started;
        char total[16];
        char received[16];
        char speed[16];
        format_size(last_total ? last_total : last_received, total, sizeof(total));
        format_size(last_received, received, sizeof(received));
        format_size(elapsed > 0 ? static_cast<u64>(static_cast<double>(last_received) / elapsed) : 0, speed, sizeof(speed));
        int const percent = last_total ? static_cast<int>(last_received * 100 / last_total) : (final_line ? 100 : 0);
        fprintf(stderr, "\r%3d %5s %3d %5s    0     0  %5s      0 --:--:-- --:--:-- --:--:-- %5s", percent, total, percent, received, speed, speed);
        if (final_line)
            fprintf(stderr, "\n");
    };

    options.on_head = [&](HTTP::ResponseHead const& head) -> ErrorOr<void> {
        if (arguments.fail_on_http_error && head.status >= 400)
            return {};
        TRY(open_output());
        if (arguments.show_headers || arguments.head_only) {
            StringBuilder text;
            text.appendff("{} {} {}\r\n", head.version.view(), head.status, head.reason.view());
            for (auto const& header : head.headers)
                text.appendff("{}: {}\r\n", header.name.view(), header.value.view());
            text.append("\r\n"_sv);
            auto const view = text.view();
            TRY(write_output({ reinterpret_cast<u8 const*>(view.characters_without_null_termination()), view.length() }));
        }
        return {};
    };
    int status_for_body = 0;
    options.on_body = [&](ReadonlyBytes bytes) -> ErrorOr<void> {
        if (arguments.fail_on_http_error && status_for_body >= 400)
            return {};
        TRY(open_output());
        return write_output(bytes);
    };
    if (arguments.verbose) {
        options.on_verbose = [](char prefix, StringView line) {
            fprintf(stderr, "%c %.*s\n", prefix, static_cast<int>(line.length()), line.characters_without_null_termination());
        };
    }
    options.on_progress = [&](u64 received, u64 total) {
        last_received = received;
        last_total = total;
    };
    if (show_progress) {
        fprintf(stderr, "  %% Total    %% Received %% Xferd  Average Speed   Time    Time     Time  Current\n");
        fprintf(stderr, "                                 Dload  Upload   Total   Spent    Left  Speed\n");
        options.on_progress = [&](u64 received, u64 total) {
            last_received = received;
            last_total = total;
            double const now = now_seconds();
            if (now - last_shown >= 0.5) {
                last_shown = now;
                show_meter(false);
            }
        };
    }

    HTTP::ResponseHead head;
    HTTP::Failure failure;
    bool const ok = HTTP::fetch(request, options, head, failure);
    if (show_progress)
        show_meter(true);
    if (file_opened)
        close(output_fd);
    if (!ok)
        return fail(failure.code, failure.message.view());
    if (arguments.write_out) {
        // %{http_code} and %{size_download}, and \\n.
        for (char const* at = arguments.write_out; *at; ++at) {
            if (strncmp(at, "%{http_code}", 12) == 0) {
                printf("%03d", head.status);
                at += 11;
            } else if (strncmp(at, "%{size_download}", 16) == 0) {
                printf("%llu", static_cast<unsigned long long>(last_received));
                at += 15;
            } else if (at[0] == '\\' && at[1] == 'n') {
                putchar('\n');
                ++at;
            } else {
                putchar(*at);
            }
        }
        fflush(stdout);
    }
    if (arguments.fail_on_http_error && head.status >= 400) {
        StringBuilder message;
        message.appendff("The requested URL returned error: {}", head.status);
        return fail(22, message.view());
    }
    return 0;
}
