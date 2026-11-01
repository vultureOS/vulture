/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <ctype.h>
#include <signal.h>
#include <strings.h>

// kill [-s signal | -signal] pid... / kill -l
static int signal_number(char const* name)
{
    if (strncasecmp(name, "SIG", 3) == 0)
        name += 3;
    for (int number = 1; number < NSIG; ++number) {
        if (strcasecmp(sys_signame[number], name) == 0)
            return number;
    }
    char* end;
    long number = strtol(name, &end, 10);
    return *end || end == name || number < 0 || number >= NSIG ? -1 : static_cast<int>(number);
}

// The names, upper case, in lines of at most 72 columns; or one signal's
// name (from its number, or an exit status of 128 + number).
static int list_signals(char const* which)
{
    if (which) {
        long number = strtol(which, nullptr, 10);
        if (number > 128)
            number -= 128;
        if (number <= 0 || number >= NSIG) {
            fprintf(stderr, "kill: unknown signal %s; valid signals:\n", which);
            return 1;
        }
        for (char const* c = sys_signame[number]; *c; ++c)
            putchar(toupper(*c));
        putchar('\n');
        return 0;
    }
    size_t column = 0;
    for (int number = 1; number < NSIG; ++number) {
        size_t length = strlen(sys_signame[number]) + 1;
        if (column > 0 && column + length > 72) {
            putchar('\n');
            column = 0;
        } else if (column > 0) {
            putchar(' ');
        }
        for (char const* c = sys_signame[number]; *c; ++c)
            putchar(toupper(*c));
        column += length;
    }
    putchar('\n');
    return 0;
}

int main(int argc, char** argv)
{
    int signal = SIGTERM;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-l") == 0)
        return list_signals(argc > 2 ? argv[2] : nullptr);
    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        signal = signal_number(argv[2]);
        first = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        signal = signal_number(argv[1] + 1);
        first = 2;
    }
    if (signal < 0 || first >= argc) {
        fprintf(stderr, "usage: kill [-s signal_name] pid ...\n       kill -l [exit_status]\n       kill -signal_name pid ...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        char* end;
        long pid = strtol(argv[i], &end, 10);
        if (*end || end == argv[i]) {
            fprintf(stderr, "kill: illegal process id: %s\n", argv[i]);
            status = 1;
            continue;
        }
        if (kill(static_cast<pid_t>(pid), signal) < 0) {
            fprintf(stderr, "kill: %ld: %s\n", pid, strerror(errno));
            status = 1;
        }
    }
    return status;
}
