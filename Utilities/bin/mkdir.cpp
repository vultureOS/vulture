/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <sys/stat.h>

// mkdir [-pv] [-m mode] directory...
int main(int argc, char** argv)
{
    bool parents = false, verbose = false;
    mode_t mode = 0777;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "-m") == 0 && first + 1 < argc) {
            mode = static_cast<mode_t>(strtol(argv[++first], nullptr, 8));
            continue;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'p')
                parents = true;
            else if (*option == 'v')
                verbose = true;
            else {
                fprintf(stderr, "usage: mkdir [-pv] [-m mode] directory_name ...\n");
                return 1;
            }
        }
    }
    if (first == argc) {
        fprintf(stderr, "usage: mkdir [-pv] [-m mode] directory_name ...\n");
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        if (!parents) {
            if (mkdir(argv[i], mode) < 0) {
                warn("mkdir", argv[i]);
                status = 1;
            } else if (verbose) {
                printf("mkdir: created directory '%s'\n", argv[i]);
            }
            continue;
        }
        char* path = strdup(argv[i]);
        for (char* cursor = path + 1;; ++cursor) {
            bool end = *cursor == '\0';
            if (*cursor == '/' || end) {
                char saved = *cursor;
                *cursor = '\0';
                struct stat information;
                if (stat(path, &information) < 0) {
                    if (mkdir(path, end ? mode : 0777) < 0) {
                        warn("mkdir", path);
                        status = 1;
                        break;
                    }
                    if (verbose)
                        printf("mkdir: created directory '%s'\n", path);
                } else if (!S_ISDIR(information.st_mode)) {
                    fprintf(stderr, "mkdir: %s: Not a directory\n", path);
                    status = 1;
                    break;
                }
                *cursor = saved;
            }
            if (end)
                break;
        }
        free(path);
    }
    return status;
}
