/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Small helpers shared by the command-line tools.

template<typename T>
constexpr T max(T a, T b) { return a < b ? b : a; }
template<typename T>
constexpr T min(T a, T b) { return a < b ? a : b; }

// "tool: what: strerror(errno)" to standard error.
inline void warn(char const* tool, char const* what)
{
    fprintf(stderr, "%s: %s: %s\n", tool, what, strerror(errno));
}

// Joins a directory and a name with one slash.
inline char* path_join(char const* directory, char const* name)
{
    size_t length = strlen(directory) + strlen(name) + 2;
    auto* path = static_cast<char*>(malloc(length));
    bool slash = length > 2 && directory[strlen(directory) - 1] == '/';
    snprintf(path, length, slash ? "%s%s" : "%s/%s", directory, name);
    return path;
}

inline char const* last_component(char const* path)
{
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        --length;
    char const* end = path + length;
    char const* start = end;
    while (start > path && start[-1] != '/')
        --start;
    static char component[1024];
    size_t size = static_cast<size_t>(end - start);
    if (size >= sizeof(component))
        size = sizeof(component) - 1;
    memcpy(component, start, size);
    component[size] = '\0';
    return component;
}
