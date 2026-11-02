/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// open [-a application] [path...]: as a double click in Files would.

int main(int argc, char** argv)
{
    StringView application;
    int first = 1;
    if (argc >= 3 && strcmp(argv[1], "-a") == 0) {
        application = StringView(argv[2], strlen(argv[2]));
        first = 3;
    }
    if (application.is_empty() && first >= argc) {
        fprintf(stderr, "usage: open [-a application] [path...]\n");
        return 64;
    }
    // What it opens is not this terminal's business, nor is what it has to say. Errors
    // of our own still go to the terminal, by a descriptor it does not inherit.
    int errors = dup(STDERR_FILENO);
    if (errors >= 0)
        fcntl(errors, F_SETFD, FD_CLOEXEC);
    int nothing = ::open("/dev/null", O_RDWR);
    if (nothing >= 0) {
        dup2(nothing, STDIN_FILENO);
        dup2(nothing, STDOUT_FILENO);
        dup2(nothing, STDERR_FILENO);
        if (nothing > STDERR_FILENO)
            close(nothing);
    }
    auto complain = [&](char const* first_part, char const* second_part) {
        if (errors < 0)
            return;
        char line[512];
        int length = snprintf(line, sizeof(line), "open: %s: %s\n", first_part, second_part);
        if (length > 0)
            (void)write(errors, line, static_cast<size_t>(min(length, static_cast<int>(sizeof(line)))));
    };
    if (!application.is_empty()) {
        auto applications = Desktop::installed_applications();
        for (auto const& candidate : applications) {
            if (candidate.name.view() != application && candidate.identifier.view() != application)
                continue;
            Vector<StringView> paths;
            for (int i = first; i < argc; ++i)
                paths.append(StringView(argv[i], strlen(argv[i])));
            auto launched = Desktop::launch(candidate, paths.span());
            if (launched.is_error()) {
                complain(argv[2], strerror(launched.error().code()));
                return 1;
            }
            return 0;
        }
        complain(argv[2], "no application by that name");
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        auto opened = Desktop::open_path(StringView(argv[i], strlen(argv[i])));
        if (opened.is_error()) {
            complain(argv[i], strerror(opened.error().code()));
            status = 1;
        }
    }
    return status;
}
