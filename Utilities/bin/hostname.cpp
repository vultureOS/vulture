/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <unistd.h>

// hostname [-s] [name]
int main(int argc, char** argv)
{
    bool short_name = argc > 1 && strcmp(argv[1], "-s") == 0;
    int first = short_name ? 2 : 1;
    if (first < argc) {
        if (sethostname(argv[first], strlen(argv[first])) < 0) {
            perror("hostname");
            return 1;
        }
        return 0;
    }
    char name[256];
    if (gethostname(name, sizeof(name)) < 0) {
        perror("hostname");
        return 1;
    }
    if (short_name) {
        if (char* dot = strchr(name, '.'))
            *dot = '\0';
    }
    printf("%s\n", name);
    return 0;
}
