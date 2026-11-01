/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// rm [-dfiRrv] file...
static bool s_force = false, s_interactive = false, s_recursive = false, s_directories = false, s_verbose = false;

static bool confirm(char const* what, char const* path)
{
    fprintf(stderr, "remove %s%s? ", what, path);
    char answer[16];
    return fgets(answer, sizeof(answer), stdin) && (answer[0] == 'y' || answer[0] == 'Y');
}

static bool remove_path(char const* path)
{
    struct stat information;
    if (lstat(path, &information) < 0) {
        if (s_force && errno == ENOENT)
            return true;
        warn("rm", path);
        return false;
    }
    if (S_ISDIR(information.st_mode)) {
        if (!s_recursive && !s_directories) {
            fprintf(stderr, "rm: %s: is a directory\n", path);
            return false;
        }
        bool ok = true;
        if (s_recursive) {
            DIR* directory = opendir(path);
            if (!directory) {
                warn("rm", path);
                return false;
            }
            // Names first (removing while reading would skip entries).
            size_t count = 0, capacity = 32;
            auto** names = static_cast<char**>(malloc(capacity * sizeof(char*)));
            while (struct dirent* entry = readdir(directory)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                if (count == capacity) {
                    capacity *= 2;
                    names = static_cast<char**>(realloc(names, capacity * sizeof(char*)));
                }
                names[count++] = path_join(path, entry->d_name);
            }
            closedir(directory);
            for (size_t i = 0; i < count; ++i) {
                ok = remove_path(names[i]) && ok;
                free(names[i]);
            }
            free(names);
        }
        if (s_interactive && !confirm("directory ", path))
            return ok;
        if (rmdir(path) < 0) {
            warn("rm", path);
            return false;
        }
        if (s_verbose)
            printf("%s\n", path);
        return ok;
    }
    if (s_interactive && !confirm("", path))
        return true;
    if (unlink(path) < 0) {
        warn("rm", path);
        return false;
    }
    if (s_verbose)
        printf("%s\n", path);
    return true;
}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'f':
                s_force = true;
                s_interactive = false;
                break;
            case 'i':
                s_interactive = true;
                s_force = false;
                break;
            case 'r':
            case 'R':
                s_recursive = true;
                break;
            case 'd':
                s_directories = true;
                break;
            case 'v':
                s_verbose = true;
                break;
            default:
                fprintf(stderr, "usage: rm [-f | -i] [-dRrv] file ...\n");
                return 1;
            }
        }
    }
    if (first == argc) {
        if (s_force)
            return 0;
        fprintf(stderr, "usage: rm [-f | -i] [-dRrv] file ...\n");
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        if (strcmp(last_component(argv[i]), ".") == 0 || strcmp(last_component(argv[i]), "..") == 0) {
            fprintf(stderr, "rm: \".\" and \"..\" may not be removed\n");
            status = 1;
            continue;
        }
        if (!remove_path(argv[i]))
            status = 1;
    }
    return status;
}
