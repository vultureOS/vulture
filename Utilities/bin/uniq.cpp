/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"

// uniq [-c] [-d | -u] [-i] [input [output]]
int main(int argc, char** argv)
{
    bool count = false, repeated_only = false, unique_only = false, ignore_case = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'c')
                count = true;
            else if (*option == 'd')
                repeated_only = true;
            else if (*option == 'u')
                unique_only = true;
            else if (*option == 'i')
                ignore_case = true;
            else {
                fprintf(stderr, "usage: uniq [-c] [-d | -u] [-i] [input [output]]\n");
                return 1;
            }
        }
    }
    FILE* in = first < argc && strcmp(argv[first], "-") != 0 ? fopen(argv[first], "r") : stdin;
    FILE* out = first + 1 < argc ? fopen(argv[first + 1], "w") : stdout;
    if (!in || !out) {
        warn("uniq", !in ? argv[first] : argv[first + 1]);
        return 1;
    }
    char* previous = nullptr;
    long repeats = 0;
    auto flush_line = [&]() {
        if (!previous)
            return;
        if ((repeated_only && repeats < 2) || (unique_only && repeats > 1))
            return;
        if (count)
            fprintf(out, "%4ld %s", repeats, previous);
        else
            fputs(previous, out);
    };
    char* line = nullptr;
    size_t size = 0;
    while (getline(&line, &size, in) >= 0) {
        bool same = previous && (ignore_case ? strcasecmp(previous, line) : strcmp(previous, line)) == 0;
        if (same) {
            ++repeats;
            continue;
        }
        flush_line();
        free(previous);
        previous = strdup(line);
        repeats = 1;
    }
    flush_line();
    fflush(out);
    return 0;
}
