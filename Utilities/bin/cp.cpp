/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// cp [-Rfipv] source target / cp [-Rfipv] source... directory
static bool s_recursive = false, s_force = false, s_interactive = false, s_preserve = false, s_verbose = false;
static int s_status = 0;

static bool confirm(char const* target)
{
    fprintf(stderr, "overwrite %s? (y/n [n]) ", target);
    char answer[16];
    return fgets(answer, sizeof(answer), stdin) && (answer[0] == 'y' || answer[0] == 'Y');
}

static bool copy_file(char const* source, char const* target, struct stat const& information)
{
    if (s_interactive && access(target, F_OK) == 0 && !confirm(target))
        return true;
    int in = open(source, O_RDONLY);
    if (in < 0) {
        warn("cp", source);
        return false;
    }
    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC, information.st_mode & 0777);
    if (out < 0 && s_force) {
        unlink(target);
        out = open(target, O_WRONLY | O_CREAT | O_TRUNC, information.st_mode & 0777);
    }
    if (out < 0) {
        warn("cp", target);
        close(in);
        return false;
    }
    char buffer[65536];
    bool ok = true;
    for (;;) {
        ssize_t got = read(in, buffer, sizeof(buffer));
        if (got == 0)
            break;
        if (got < 0) {
            warn("cp", source);
            ok = false;
            break;
        }
        for (ssize_t done = 0; done < got;) {
            ssize_t written = write(out, buffer + done, static_cast<size_t>(got - done));
            if (written <= 0) {
                warn("cp", target);
                ok = false;
                break;
            }
            done += written;
        }
        if (!ok)
            break;
    }
    if (s_preserve) {
        fchmod(out, information.st_mode & 07777);
        struct timespec times[2] = { information.st_atim, information.st_mtim };
        futimens(out, times);
        if (fchown(out, information.st_uid, information.st_gid) < 0) {
        }
    }
    close(in);
    close(out);
    if (ok && s_verbose)
        printf("%s -> %s\n", source, target);
    return ok;
}

static bool copy(char const* source, char const* target)
{
    struct stat information;
    if (lstat(source, &information) < 0) {
        warn("cp", source);
        return false;
    }
    if (S_ISLNK(information.st_mode) && s_recursive) {
        char link_target[1024];
        ssize_t length = readlink(source, link_target, sizeof(link_target) - 1);
        if (length < 0)
            return false;
        link_target[length] = '\0';
        unlink(target);
        if (symlink(link_target, target) < 0) {
            warn("cp", target);
            return false;
        }
        return true;
    }
    if (S_ISLNK(information.st_mode))
        stat(source, &information);
    if (S_ISDIR(information.st_mode)) {
        if (!s_recursive) {
            fprintf(stderr, "cp: %s is a directory (not copied).\n", source);
            return false;
        }
        if (mkdir(target, information.st_mode & 0777) < 0 && errno != EEXIST) {
            warn("cp", target);
            return false;
        }
        DIR* directory = opendir(source);
        if (!directory) {
            warn("cp", source);
            return false;
        }
        bool ok = true;
        while (struct dirent* entry = readdir(directory)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char* from = path_join(source, entry->d_name);
            char* to = path_join(target, entry->d_name);
            ok = copy(from, to) && ok;
            free(from);
            free(to);
        }
        closedir(directory);
        if (s_preserve)
            chmod(target, information.st_mode & 07777);
        return ok;
    }
    return copy_file(source, target, information);
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
            case 'R':
            case 'r':
                s_recursive = true;
                break;
            case 'f':
                s_force = true;
                s_interactive = false;
                break;
            case 'i':
                s_interactive = true;
                s_force = false;
                break;
            case 'p':
                s_preserve = true;
                break;
            case 'a':
                s_recursive = s_preserve = true;
                break;
            case 'v':
                s_verbose = true;
                break;
            default:
                fprintf(stderr, "usage: cp [-R] [-fi] [-apv] source_file target_file\n       cp [-R] [-fi] [-apv] source_file ... target_directory\n");
                return 1;
            }
        }
    }
    int count = argc - first;
    if (count < 2) {
        fprintf(stderr, "usage: cp [-R] [-fi] [-apv] source_file target_file\n       cp [-R] [-fi] [-apv] source_file ... target_directory\n");
        return 1;
    }
    char const* target = argv[argc - 1];
    struct stat target_information;
    bool target_is_directory = stat(target, &target_information) == 0 && S_ISDIR(target_information.st_mode);
    if (count > 2 && !target_is_directory) {
        fprintf(stderr, "cp: %s is not a directory\n", target);
        return 1;
    }
    for (int i = first; i < argc - 1; ++i) {
        char* destination = target_is_directory ? path_join(target, last_component(argv[i])) : strdup(target);
        if (!copy(argv[i], destination))
            s_status = 1;
        free(destination);
    }
    return s_status;
}
