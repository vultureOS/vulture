/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

// id [-Ggnu] [user]; also whoami and groups (by name).
int main(int argc, char** argv)
{
    char const* program = last_component(argv[0]);
    bool only_user = strcmp(program, "whoami") == 0, only_group = false, all_groups = strcmp(program, "groups") == 0, names = only_user || all_groups;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'u')
                only_user = true;
            else if (*option == 'g')
                only_group = true;
            else if (*option == 'G')
                all_groups = true;
            else if (*option == 'n')
                names = true;
        }
    }
    uid_t uid = geteuid(), real_uid = getuid();
    gid_t gid = getegid();
    gid_t groups[16];
    int group_count = getgroups(16, groups);
    char const* user_name = nullptr;
    if (first < argc) {
        struct passwd* user = getpwnam(argv[first]);
        if (!user) {
            fprintf(stderr, "id: %s: no such user\n", argv[first]);
            return 1;
        }
        uid = real_uid = user->pw_uid;
        gid = user->pw_gid;
        group_count = 16;
        getgrouplist(user->pw_name, gid, groups, &group_count);
        user_name = user->pw_name;
    } else if (group_count <= 0) {
        groups[0] = gid;
        group_count = 1;
    }
    auto user_text = [](uid_t id) -> char const* {
        struct passwd* user = getpwuid(id);
        return user ? user->pw_name : nullptr;
    };
    auto group_text = [](gid_t id) -> char const* {
        struct group* group = getgrgid(id);
        return group ? group->gr_name : nullptr;
    };
    (void)user_name;
    if (only_user) {
        char const* name = user_text(uid);
        names && name ? printf("%s\n", name) : printf("%u\n", uid);
        return 0;
    }
    if (only_group) {
        char const* name = group_text(gid);
        names && name ? printf("%s\n", name) : printf("%u\n", gid);
        return 0;
    }
    if (all_groups) {
        for (int i = 0; i < group_count; ++i) {
            char const* name = group_text(groups[i]);
            if (i > 0)
                putchar(' ');
            names && name ? printf("%s", name) : printf("%u", groups[i]);
        }
        putchar('\n');
        return 0;
    }
    char const* name = user_text(real_uid);
    printf("uid=%u(%s) gid=%u(%s)", real_uid, name ? name : "?", gid, group_text(gid) ? group_text(gid) : "?");
    if (uid != real_uid)
        printf(" euid=%u(%s)", uid, user_text(uid) ? user_text(uid) : "?");
    printf(" groups=");
    for (int i = 0; i < group_count; ++i)
        printf("%s%u(%s)", i ? "," : "", groups[i], group_text(groups[i]) ? group_text(groups[i]) : "?");
    putchar('\n');
    return 0;
}
