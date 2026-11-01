/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <unistd.h>

// rmdir [-pv] directory...
int main(int argc, char** argv)
{
    bool parents = false, verbose = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'p')
                parents = true;
            else if (*option == 'v')
                verbose = true;
            else {
                fprintf(stderr, "usage: rmdir [-pv] directory ...\n");
                return 1;
            }
        }
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        char* path = strdup(argv[i]);
        for (;;) {
            if (rmdir(path) < 0) {
                warn("rmdir", path);
                status = 1;
                break;
            }
            if (verbose)
                printf("%s\n", path);
            if (!parents)
                break;
            char* slash = strrchr(path, '/');
            if (!slash || slash == path)
                break;
            *slash = '\0';
        }
        free(path);
    }
    return status;
}
