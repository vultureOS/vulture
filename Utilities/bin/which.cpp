/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <sys/stat.h>
#include <unistd.h>

// which [-as] program...: where PATH finds it.
int main(int argc, char** argv)
{
    bool all = false, silent = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'a')
                all = true;
            else if (*option == 's')
                silent = true;
        }
    }
    char const* path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin:/sbin:/usr/sbin";
    int status = 0;
    for (int i = first; i < argc; ++i) {
        bool found = false;
        char* copy = strdup(path);
        char* state = nullptr;
        for (char* directory = strtok_r(copy, ":", &state); directory; directory = strtok_r(nullptr, ":", &state)) {
            char* candidate = path_join(*directory ? directory : ".", argv[i]);
            struct stat information;
            if (stat(candidate, &information) == 0 && S_ISREG(information.st_mode) && access(candidate, X_OK) == 0) {
                if (!silent)
                    printf("%s\n", candidate);
                found = true;
            }
            free(candidate);
            if (found && !all)
                break;
        }
        free(copy);
        if (!found)
            status = 1;
    }
    return status;
}
