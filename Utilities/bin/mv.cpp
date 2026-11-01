/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// mv [-fiv] source target / mv [-fiv] source... directory. Across
// filesystems: copy (cp -pR), then remove (rm -rf).
static bool s_interactive = false, s_verbose = false, s_no_clobber = false;

static int run(char const* const argv[])
{
    pid_t child = fork();
    if (child == 0) {
        execv(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static bool move_one(char const* source, char const* target)
{
    if (access(target, F_OK) == 0) {
        if (s_no_clobber)
            return true;
        if (s_interactive) {
            fprintf(stderr, "overwrite %s? (y/n [n]) ", target);
            char answer[16];
            if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y'))
                return true;
        }
    }
    if (rename(source, target) == 0) {
        if (s_verbose)
            printf("%s -> %s\n", source, target);
        return true;
    }
    if (errno != EXDEV) {
        fprintf(stderr, "mv: rename %s to %s: %s\n", source, target, strerror(errno));
        return false;
    }
    char const* copy[] = { "/bin/cp", "-pR", source, target, nullptr };
    if (run(copy) != 0)
        return false;
    char const* remove[] = { "/bin/rm", "-rf", source, nullptr };
    if (run(remove) != 0)
        return false;
    if (s_verbose)
        printf("%s -> %s\n", source, target);
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
            if (*option == 'f') {
                s_interactive = s_no_clobber = false;
            } else if (*option == 'i') {
                s_interactive = true;
            } else if (*option == 'n') {
                s_no_clobber = true;
            } else if (*option == 'v') {
                s_verbose = true;
            } else {
                fprintf(stderr, "usage: mv [-f | -i | -n] [-v] source target\n       mv [-f | -i | -n] [-v] source ... directory\n");
                return 1;
            }
        }
    }
    if (argc - first < 2) {
        fprintf(stderr, "usage: mv [-f | -i | -n] [-v] source target\n       mv [-f | -i | -n] [-v] source ... directory\n");
        return 1;
    }
    char const* target = argv[argc - 1];
    struct stat information;
    bool directory = stat(target, &information) == 0 && S_ISDIR(information.st_mode);
    if (argc - first > 2 && !directory) {
        fprintf(stderr, "mv: %s is not a directory\n", target);
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc - 1; ++i) {
        char* destination = directory ? path_join(target, last_component(argv[i])) : strdup(target);
        if (!move_one(argv[i], destination))
            status = 1;
        free(destination);
    }
    return status;
}
