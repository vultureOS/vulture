/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"
#include <LibBase/Error.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// What the tools that deal in whole trees of files have in common: tar, zip, unzip and ditto.

inline ErrorOr<Vector<u8>> read_whole_file(char const* path)
{
    int const fd = strcmp(path, "-") == 0 ? STDIN_FILENO : open(path, O_RDONLY);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> data;
    char chunk[65536];
    for (;;) {
        ssize_t const got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            int const saved = errno;
            if (fd != STDIN_FILENO)
                close(fd);
            return Error::from_errno(saved);
        }
        if (got == 0)
            break;
        size_t const at = data.size();
        TRY(data.try_resize(at + static_cast<size_t>(got)));
        memcpy(data.data() + at, chunk, static_cast<size_t>(got));
    }
    if (fd != STDIN_FILENO)
        close(fd);
    return data;
}

inline bool write_all_to(int fd, void const* data, size_t size)
{
    auto const* bytes = static_cast<char const*>(data);
    size_t done = 0;
    while (done < size) {
        ssize_t const wrote = write(fd, bytes + done, size - done);
        if (wrote < 0)
            return false;
        done += static_cast<size_t>(wrote);
    }
    return true;
}

// Every directory above a path that's about to be made.
inline void make_parents(char const* path)
{
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char* slash = strchr(copy + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        (void)mkdir(copy, 0755);
        *slash = '/';
    }
}

// A file, with these contents and this mode, in place of whatever's there.
inline bool write_whole_file(char const* path, void const* data, size_t size, mode_t mode)
{
    (void)unlink(path);
    int const fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return false;
    bool const wrote = write_all_to(fd, data, size);
    close(fd);
    return wrote;
}

// Depth first, a directory before what's in it, in the order readdir gives them. visit(path relative to `base`, full path, status).
template<typename Visit>
void walk_tree(char const* full, char const* relative, Visit visit)
{
    struct stat status;
    if (lstat(full, &status) != 0) {
        fprintf(stderr, "%s: %s\n", full, strerror(errno));
        return;
    }
    visit(relative, full, status);
    if (!S_ISDIR(status.st_mode))
        return;
    DIR* directory = opendir(full);
    if (!directory) {
        fprintf(stderr, "%s: %s\n", full, strerror(errno));
        return;
    }
    while (auto* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child_full[1024], child_relative[1024];
        snprintf(child_full, sizeof(child_full), "%s/%s", full, entry->d_name);
        if (relative[0])
            snprintf(child_relative, sizeof(child_relative), "%s/%s", relative, entry->d_name);
        else
            snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name);
        walk_tree(child_full, child_relative, visit);
    }
    closedir(directory);
}

inline void to_dos_time(time_t when, u16& date, u16& clock)
{
    tm parts {};
    localtime_r(&when, &parts);
    int year = parts.tm_year - 80;
    if (year < 0) {
        date = (1 << 5) | 1;
        clock = 0;
        return;
    }
    date = static_cast<u16>(year << 9 | (parts.tm_mon + 1) << 5 | parts.tm_mday);
    clock = static_cast<u16>(parts.tm_hour << 11 | parts.tm_min << 5 | parts.tm_sec / 2);
}
