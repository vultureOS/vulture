/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <os/log.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// logger [-is] [-p pri] [-t tag] [message ...]: into the unified log (each
// line of standard input without a message). Priorities map onto os_log's
// types: emerg..err are errors, debug is debug, info is info, the rest
// default.

namespace {

os_log_type_t type_for(char const* priority)
{
    char const* level = strchr(priority, '.');
    level = level ? level + 1 : priority;
    char const* const errors[] = { "emerg", "panic", "alert", "crit", "err", "error" };
    for (auto const* name : errors) {
        if (strcmp(level, name) == 0)
            return OS_LOG_TYPE_ERROR;
    }
    if (strcmp(level, "debug") == 0)
        return OS_LOG_TYPE_DEBUG;
    if (strcmp(level, "info") == 0)
        return OS_LOG_TYPE_INFO;
    return OS_LOG_TYPE_DEFAULT;
}

void send(os_log_type_t type, char const* tag, bool to_stderr, char const* message)
{
    if (tag)
        os_log_with_type(OS_LOG_DEFAULT, type, "%s: %s", tag, message);
    else
        os_log_with_type(OS_LOG_DEFAULT, type, "%s", message);
    if (to_stderr)
        fprintf(stderr, "%s%s%s\n", tag ? tag : "", tag ? ": " : "", message);
}

}

int main(int argc, char** argv)
{
    os_log_type_t type = OS_LOG_TYPE_DEFAULT;
    char const* tag = nullptr;
    bool to_stderr = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            ++i;
            break;
        }
        for (char const* flag = argv[i] + 1; *flag; ++flag) {
            if (*flag == 'i') {
                continue; // The pid is always logged.
            } else if (*flag == 's') {
                to_stderr = true;
            } else if ((*flag == 'p' || *flag == 't') && i + 1 < argc && flag[1] == '\0') {
                if (*flag == 'p')
                    type = type_for(argv[++i]);
                else
                    tag = argv[++i];
                break;
            } else {
                fprintf(stderr, "usage: logger [-is] [-p pri] [-t tag] [message ...]\n");
                return 1;
            }
        }
    }
    if (i < argc) {
        char message[1024] = {};
        size_t length = 0;
        for (; i < argc; ++i) {
            int wrote = snprintf(message + length, sizeof(message) - length, "%s%s", length ? " " : "", argv[i]);
            if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(message) - length)
                break;
            length += static_cast<size_t>(wrote);
        }
        send(type, tag, to_stderr, message);
        return 0;
    }
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        send(type, tag, to_stderr, line);
    }
    return 0;
}
