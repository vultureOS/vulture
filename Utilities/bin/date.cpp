/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <locale.h>
#include <time.h>

// date [-u] [-r seconds] [+format]: the time, as macOS prints it
// ("Sun Nov  1 12:34:56 UTC 2026").
int main(int argc, char** argv)
{
    bool utc = false;
    time_t when = time(nullptr);
    setlocale(LC_ALL, "");
    char const* format = "%+";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-u") == 0) {
            utc = true;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            when = static_cast<time_t>(strtoll(argv[++i], nullptr, 10));
        } else if (argv[i][0] == '+') {
            format = argv[i] + 1;
        } else {
            fprintf(stderr, "usage: date [-u] [-r seconds] [+format]\n");
            return 1;
        }
    }
    struct tm broken;
    if (utc)
        gmtime_r(&when, &broken);
    else
        localtime_r(&when, &broken);
    char text[256];
    strftime(text, sizeof(text), format, &broken);
    printf("%s\n", text);
    return 0;
}
