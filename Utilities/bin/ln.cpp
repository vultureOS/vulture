/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <sys/stat.h>
#include <unistd.h>

// ln [-fsv] source [target] / ln [-fsv] source... directory
int main(int argc, char** argv)
{
    bool symbolic = false, force = false, verbose = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 's')
                symbolic = true;
            else if (*option == 'f')
                force = true;
            else if (*option == 'v')
                verbose = true;
            else if (*option == 'n' || *option == 'h')
                continue;
            else {
                fprintf(stderr, "usage: ln [-fsv] source_file [target_file]\n       ln [-fsv] source_file ... target_dir\n");
                return 1;
            }
        }
    }
    int count = argc - first;
    if (count < 1) {
        fprintf(stderr, "usage: ln [-fsv] source_file [target_file]\n       ln [-fsv] source_file ... target_dir\n");
        return 1;
    }
    char const* target = count == 1 ? "." : argv[argc - 1];
    struct stat information;
    bool directory = stat(target, &information) == 0 && S_ISDIR(information.st_mode);
    int sources_end = count == 1 ? argc : argc - 1;
    int status = 0;
    for (int i = first; i < sources_end; ++i) {
        char* destination = directory ? path_join(target, last_component(argv[i])) : strdup(target);
        if (force)
            unlink(destination);
        int result = symbolic ? symlink(argv[i], destination) : link(argv[i], destination);
        if (result < 0) {
            warn("ln", destination);
            status = 1;
        } else if (verbose) {
            printf("%s %s %s\n", destination, symbolic ? "->" : "=>", argv[i]);
        }
        free(destination);
    }
    return status;
}
