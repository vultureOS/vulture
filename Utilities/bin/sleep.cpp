/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <time.h>

// sleep seconds: fractions allowed, and suffixes s m h d.
int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 1;
    }
    double total = 0;
    for (int i = 1; i < argc; ++i) {
        char* end;
        double value = strtod(argv[i], &end);
        if (end == argv[i] || value < 0) {
            fprintf(stderr, "usage: sleep seconds\n");
            return 1;
        }
        if (*end == 'm')
            value *= 60;
        else if (*end == 'h')
            value *= 3600;
        else if (*end == 'd')
            value *= 86400;
        total += value;
    }
    struct timespec duration;
    duration.tv_sec = static_cast<time_t>(total);
    duration.tv_nsec = static_cast<long>((total - static_cast<double>(duration.tv_sec)) * 1e9);
    while (nanosleep(&duration, nullptr) < 0 && errno == EINTR) {
    }
    return 0;
}
