/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <libgen.h>
#include <string.h>

extern "C" {

char* basename(char* path)
{
    if (!path || !*path)
        return const_cast<char*>(".");
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        path[--length] = '\0';
    char* slash = strrchr(path, '/');
    if (!slash || length == 1)
        return path;
    return slash + 1;
}

char* dirname(char* path)
{
    if (!path || !*path)
        return const_cast<char*>(".");
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        --length;
    while (length > 0 && path[length - 1] != '/')
        --length;
    if (length == 0)
        return const_cast<char*>(".");
    while (length > 1 && path[length - 1] == '/')
        --length;
    path[length] = '\0';
    return path;
}
}
