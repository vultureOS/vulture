/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// mkfifo [-m mode] file...
int main(int argc, char** argv)
{
    mode_t mode = 0666;
    int option;
    while ((option = getopt(argc, argv, "m:")) != -1) {
        if (option == 'm') {
            mode = static_cast<mode_t>(strtol(optarg, nullptr, 8));
            continue;
        }
        fprintf(stderr, "usage: mkfifo [-m mode] fifo_name ...\n");
        return 1;
    }
    if (optind == argc) {
        fprintf(stderr, "usage: mkfifo [-m mode] fifo_name ...\n");
        return 1;
    }
    int result = 0;
    for (int i = optind; i < argc; ++i) {
        if (mkfifo(argv[i], mode) != 0) {
            fprintf(stderr, "mkfifo: %s: %s\n", argv[i], strerror(errno));
            result = 1;
        }
    }
    return result;
}
