/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// touch [-acm] [-r file] [-t [[CC]YY]MMDDhhmm[.SS]] file...
int main(int argc, char** argv)
{
    bool no_create = false, access_only = false, modification_only = false;
    struct timespec when[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "-r") == 0 && first + 1 < argc) {
            struct stat reference;
            if (stat(argv[++first], &reference) < 0) {
                warn("touch", argv[first]);
                return 1;
            }
            when[0] = reference.st_atim;
            when[1] = reference.st_mtim;
            continue;
        }
        if (strcmp(argv[first], "-t") == 0 && first + 1 < argc) {
            char const* text = argv[++first];
            struct tm broken {};
            time_t now = time(nullptr);
            localtime_r(&now, &broken);
            char digits[16] = {};
            char const* dot = strchr(text, '.');
            size_t length = dot ? static_cast<size_t>(dot - text) : strlen(text);
            if (length != 8 && length != 10 && length != 12) {
                fprintf(stderr, "touch: out of range or illegal time specification: [[CC]YY]MMDDhhmm[.SS]\n");
                return 1;
            }
            memcpy(digits, text, length);
            auto take = [&](size_t& offset) {
                int value = (digits[offset] - '0') * 10 + (digits[offset + 1] - '0');
                offset += 2;
                return value;
            };
            size_t offset = 0;
            if (length == 12)
                broken.tm_year = take(offset) * 100 - 1900 + take(offset);
            else if (length == 10)
                broken.tm_year = take(offset) + 100;
            broken.tm_mon = take(offset) - 1;
            broken.tm_mday = take(offset);
            broken.tm_hour = take(offset);
            broken.tm_min = take(offset);
            broken.tm_sec = dot ? atoi(dot + 1) : 0;
            time_t moment = mktime(&broken);
            when[0] = when[1] = { moment, 0 };
            continue;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'c')
                no_create = true;
            else if (*option == 'a')
                access_only = true;
            else if (*option == 'm')
                modification_only = true;
            else {
                fprintf(stderr, "usage: touch [-acm] [-r file] [-t [[CC]YY]MMDDhhmm[.SS]] file ...\n");
                return 1;
            }
        }
    }
    if (access_only && !modification_only)
        when[1].tv_nsec = UTIME_OMIT;
    if (modification_only && !access_only)
        when[0].tv_nsec = UTIME_OMIT;
    int status = 0;
    for (int i = first; i < argc; ++i) {
        if (utimensat(AT_FDCWD, argv[i], when, 0) == 0)
            continue;
        if (errno != ENOENT) {
            warn("touch", argv[i]);
            status = 1;
            continue;
        }
        if (no_create)
            continue;
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            warn("touch", argv[i]);
            status = 1;
            continue;
        }
        futimens(fd, when);
        close(fd);
    }
    return status;
}
