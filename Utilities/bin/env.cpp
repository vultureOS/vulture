/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <unistd.h>

extern char** environ;

// env [-i] [-u name] [name=value]... [command [argument...]]
int main(int argc, char** argv)
{
    int i = 1;
    for (; i < argc; ++i) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-") == 0) {
            clearenv();
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            unsetenv(argv[++i]);
        } else if (strchr(argv[i], '=')) {
            putenv(argv[i]);
        } else {
            break;
        }
    }
    if (i == argc) {
        for (char** entry = environ; entry && *entry; ++entry)
            printf("%s\n", *entry);
        return 0;
    }
    execvp(argv[i], argv + i);
    fprintf(stderr, "env: %s: %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
