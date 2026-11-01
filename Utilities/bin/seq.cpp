/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"

// seq [-w] [-s separator] [first [increment]] last
int main(int argc, char** argv)
{
    char const* separator = "\n";
    bool equal_width = false;
    int first_argument = 1;
    for (; first_argument < argc && argv[first_argument][0] == '-' && argv[first_argument][1] && !(argv[first_argument][1] >= '0' && argv[first_argument][1] <= '9'); ++first_argument) {
        char const* option = argv[first_argument];
        if (option[1] == 's')
            separator = option[2] ? option + 2 : (first_argument + 1 < argc ? argv[++first_argument] : "\n");
        else if (option[1] == 'w')
            equal_width = true;
    }
    int count = argc - first_argument;
    if (count < 1 || count > 3) {
        fprintf(stderr, "usage: seq [-w] [-s string] [first [incr]] last\n");
        return 1;
    }
    double first = 1, increment = 1, last;
    if (count == 1) {
        last = strtod(argv[first_argument], nullptr);
    } else if (count == 2) {
        first = strtod(argv[first_argument], nullptr);
        last = strtod(argv[first_argument + 1], nullptr);
    } else {
        first = strtod(argv[first_argument], nullptr);
        increment = strtod(argv[first_argument + 1], nullptr);
        last = strtod(argv[first_argument + 2], nullptr);
    }
    if (increment == 0) {
        fprintf(stderr, "seq: zero increment\n");
        return 1;
    }
    bool integers = first == static_cast<long long>(first) && increment == static_cast<long long>(increment);
    int width = 0;
    if (equal_width) {
        char text[64];
        width = max(snprintf(text, sizeof(text), "%.0f", first), snprintf(text, sizeof(text), "%.0f", last));
    }
    bool started = false;
    for (double value = first; increment > 0 ? value <= last + 1e-9 : value >= last - 1e-9; value += increment) {
        if (started)
            fputs(separator, stdout);
        started = true;
        if (integers)
            printf(equal_width ? "%0*.0f" : "%*.0f", width, value);
        else
            printf("%g", value);
    }
    if (started)
        putchar('\n');
    return 0;
}
