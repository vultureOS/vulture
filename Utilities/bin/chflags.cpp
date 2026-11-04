/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>

// chflags [-R] [-v] flags file...: a file's flags, by the names a Mac has for them, or as a number.

namespace {

struct Name {
    char const* name;
    unsigned bits;
};

constexpr Name s_names[] = {
    { "arch", SF_ARCHIVED },
    { "archived", SF_ARCHIVED },
    { "nodump", UF_NODUMP },
    { "opaque", UF_OPAQUE },
    { "sappnd", SF_APPEND },
    { "sappend", SF_APPEND },
    { "schg", SF_IMMUTABLE },
    { "schange", SF_IMMUTABLE },
    { "simmutable", SF_IMMUTABLE },
    { "uappnd", UF_APPEND },
    { "uappend", UF_APPEND },
    { "uchg", UF_IMMUTABLE },
    { "uchange", UF_IMMUTABLE },
    { "uimmutable", UF_IMMUTABLE },
    { "hidden", UF_HIDDEN },
};

unsigned s_set = 0;
unsigned s_clear = 0;
bool s_absolute = false;
bool s_recursive = false;
bool s_verbose = false;

bool parse_flags(char const* text)
{
    if (text[0] >= '0' && text[0] <= '7') {
        char* end = nullptr;
        s_set = static_cast<unsigned>(strtoul(text, &end, 8));
        s_absolute = true;
        return *end == 0;
    }
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", text);
    for (char* word = strtok(copy, ","); word; word = strtok(nullptr, ",")) {
        bool const negative = strncmp(word, "no", 2) == 0 && strcmp(word, "nodump") != 0;
        char const* const bare = negative ? word + 2 : word;
        bool found = false;
        // "dump" is the opposite of "nodump".
        if (strcmp(word, "dump") == 0) {
            s_clear |= UF_NODUMP;
            continue;
        }
        for (auto const& name : s_names) {
            if (strcmp(bare, name.name) == 0) {
                (negative ? s_clear : s_set) |= name.bits;
                found = true;
            }
        }
        if (!found) {
            fprintf(stderr, "chflags: invalid flag: %s\n", word);
            return false;
        }
    }
    return true;
}

bool change(char const* path)
{
    bool ok = true;
    struct stat information;
    if (stat(path, &information) != 0) {
        warn("chflags", path);
        return false;
    }
    if (s_recursive && S_ISDIR(information.st_mode)) {
        if (DIR* directory = opendir(path)) {
            while (auto* entry = readdir(directory)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                char inner[PATH_MAX];
                snprintf(inner, sizeof(inner), "%s/%s", path, entry->d_name);
                ok = change(inner) && ok;
            }
            closedir(directory);
        }
    }
    unsigned const flags = s_absolute ? s_set : (information.st_flags | s_set) & ~s_clear;
    if (chflags(path, flags) != 0) {
        warn("chflags", path);
        return false;
    }
    if (s_verbose)
        printf("%s\n", path);
    return ok;
}

}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'R')
                s_recursive = true;
            else if (*option == 'v')
                s_verbose = true;
            else if (*option != 'h' && *option != 'H' && *option != 'L' && *option != 'P' && *option != 'f') {
                fprintf(stderr, "usage: chflags [-fhv] [-R [-H | -L | -P]] flags file ...\n");
                return 1;
            }
        }
    }
    if (argc - first < 2) {
        fprintf(stderr, "usage: chflags [-fhv] [-R [-H | -L | -P]] flags file ...\n");
        return 1;
    }
    if (!parse_flags(argv[first]))
        return 1;
    int status = 0;
    for (int i = first + 1; i < argc; ++i) {
        if (!change(argv[i]))
            status = 1;
    }
    return status;
}
