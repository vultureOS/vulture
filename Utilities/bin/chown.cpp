/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

// chown [-Rhv] owner[:group] file...  (also chgrp group file...)
static bool s_recursive = false, s_no_dereference = false, s_verbose = false;

static bool change(char const* path, uid_t uid, gid_t gid)
{
    int result = s_no_dereference ? lchown(path, uid, gid) : chown(path, uid, gid);
    bool ok = true;
    if (result < 0) {
        warn("chown", path);
        ok = false;
    } else if (s_verbose) {
        printf("%s\n", path);
    }
    struct stat information;
    if (s_recursive && lstat(path, &information) == 0 && S_ISDIR(information.st_mode)) {
        DIR* directory = opendir(path);
        if (directory) {
            while (struct dirent* entry = readdir(directory)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                char* child = path_join(path, entry->d_name);
                ok = change(child, uid, gid) && ok;
                free(child);
            }
            closedir(directory);
        }
    }
    return ok;
}

static bool parse_user(char const* text, uid_t& uid)
{
    if (struct passwd* user = getpwnam(text)) {
        uid = user->pw_uid;
        return true;
    }
    char* end;
    long value = strtol(text, &end, 10);
    if (*end || end == text)
        return false;
    uid = static_cast<uid_t>(value);
    return true;
}

static bool parse_group(char const* text, gid_t& gid)
{
    if (struct group* group = getgrnam(text)) {
        gid = group->gr_gid;
        return true;
    }
    char* end;
    long value = strtol(text, &end, 10);
    if (*end || end == text)
        return false;
    gid = static_cast<gid_t>(value);
    return true;
}

int main(int argc, char** argv)
{
    bool chgrp = strcmp(last_component(argv[0]), "chgrp") == 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'R')
                s_recursive = true;
            else if (*option == 'h')
                s_no_dereference = true;
            else if (*option == 'v')
                s_verbose = true;
        }
    }
    if (argc - first < 2) {
        fprintf(stderr, chgrp ? "usage: chgrp [-Rhv] group file ...\n" : "usage: chown [-Rhv] owner[:group] file ...\n");
        return 1;
    }
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    char* spec = strdup(argv[first]);
    if (chgrp) {
        if (!parse_group(spec, gid)) {
            fprintf(stderr, "chgrp: %s: illegal group name\n", spec);
            return 1;
        }
    } else {
        char* separator = strpbrk(spec, ":.");
        if (separator) {
            *separator = '\0';
            if (separator[1] && !parse_group(separator + 1, gid)) {
                fprintf(stderr, "chown: %s: illegal group name\n", separator + 1);
                return 1;
            }
        }
        if (*spec && !parse_user(spec, uid)) {
            fprintf(stderr, "chown: %s: illegal user name\n", spec);
            return 1;
        }
    }
    int status = 0;
    for (int i = first + 1; i < argc; ++i) {
        if (!change(argv[i], uid, gid))
            status = 1;
    }
    return status;
}
