/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <dirent.h>
#include <sys/stat.h>

// chmod [-Rv] mode file...: octal (755) or symbolic (u+x,go-w,a=r,+X).
static bool s_recursive = false, s_verbose = false;

static bool apply_symbolic(char const* spec, mode_t& mode, bool is_directory)
{
    mode_t mask = umask(0);
    umask(mask);
    char const* cursor = spec;
    while (*cursor) {
        mode_t who = 0;
        for (; *cursor && strchr("ugoa", *cursor); ++cursor) {
            if (*cursor == 'u')
                who |= 04700;
            else if (*cursor == 'g')
                who |= 02070;
            else if (*cursor == 'o')
                who |= 01007;
            else
                who |= 07777;
        }
        bool implicit = who == 0;
        if (implicit)
            who = 07777;
        while (*cursor == '+' || *cursor == '-' || *cursor == '=') {
            char op = *cursor++;
            mode_t bits = 0;
            for (; *cursor && strchr("rwxXst", *cursor); ++cursor) {
                switch (*cursor) {
                case 'r':
                    bits |= 0444;
                    break;
                case 'w':
                    bits |= 0222;
                    break;
                case 'x':
                    bits |= 0111;
                    break;
                case 'X':
                    if (is_directory || (mode & 0111))
                        bits |= 0111;
                    break;
                case 's':
                    bits |= 06000;
                    break;
                case 't':
                    bits |= 01000;
                    break;
                }
            }
            bits &= who;
            if (implicit)
                bits &= ~mask;
            if (op == '+')
                mode |= bits;
            else if (op == '-')
                mode &= ~bits;
            else
                mode = (mode & ~(who & 07777)) | bits;
        }
        if (*cursor == ',')
            ++cursor;
        else if (*cursor)
            return false;
    }
    return true;
}

static bool change(char const* path, char const* spec)
{
    struct stat information;
    if (stat(path, &information) < 0) {
        warn("chmod", path);
        return false;
    }
    mode_t mode = information.st_mode & 07777;
    char* end;
    long octal = strtol(spec, &end, 8);
    if (*end == '\0' && end != spec) {
        mode = static_cast<mode_t>(octal);
    } else if (!apply_symbolic(spec, mode, S_ISDIR(information.st_mode))) {
        fprintf(stderr, "chmod: Invalid file mode: %s\n", spec);
        exit(1);
    }
    bool ok = true;
    if (chmod(path, mode) < 0) {
        warn("chmod", path);
        ok = false;
    } else if (s_verbose) {
        printf("%s\n", path);
    }
    if (s_recursive && S_ISDIR(information.st_mode)) {
        DIR* directory = opendir(path);
        if (directory) {
            while (struct dirent* entry = readdir(directory)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                char* child = path_join(path, entry->d_name);
                ok = change(child, spec) && ok;
                free(child);
            }
            closedir(directory);
        }
    }
    return ok;
}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1] && strchr("Rv", argv[first][1]); ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'R')
                s_recursive = true;
            else if (*option == 'v')
                s_verbose = true;
        }
    }
    if (argc - first < 2) {
        fprintf(stderr, "usage: chmod [-Rv] mode file ...\n");
        return 1;
    }
    int status = 0;
    for (int i = first + 1; i < argc; ++i) {
        if (!change(argv[i], argv[first]))
            status = 1;
    }
    return status;
}
