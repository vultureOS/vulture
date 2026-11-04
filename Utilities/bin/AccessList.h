/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/acl.h>

// Access control lists, in the words ls -e prints and chmod +a takes, as on a Mac.

namespace AccessList {

struct Word {
    char const* name;
    unsigned value;
};

// The order they're printed in. Which words there are depends on what it's about.
inline constexpr Word file_rights[] = {
    { "read", VULTURE_ACL_READ_DATA },
    { "write", VULTURE_ACL_WRITE_DATA },
    { "execute", VULTURE_ACL_EXECUTE },
    { "delete", VULTURE_ACL_DELETE },
    { "append", VULTURE_ACL_APPEND_DATA },
};
inline constexpr Word directory_rights[] = {
    { "list", VULTURE_ACL_LIST_DIRECTORY },
    { "add_file", VULTURE_ACL_ADD_FILE },
    { "search", VULTURE_ACL_SEARCH },
    { "delete", VULTURE_ACL_DELETE },
    { "add_subdirectory", VULTURE_ACL_ADD_SUBDIRECTORY },
    { "delete_child", VULTURE_ACL_DELETE_CHILD },
};
inline constexpr Word common_rights[] = {
    { "readattr", VULTURE_ACL_READ_ATTRIBUTES },
    { "writeattr", VULTURE_ACL_WRITE_ATTRIBUTES },
    { "readextattr", VULTURE_ACL_READ_EXTATTRIBUTES },
    { "writeextattr", VULTURE_ACL_WRITE_EXTATTRIBUTES },
    { "readsecurity", VULTURE_ACL_READ_SECURITY },
    { "writesecurity", VULTURE_ACL_WRITE_SECURITY },
    { "chown", VULTURE_ACL_CHANGE_OWNER },
};
inline constexpr Word inheritance_words[] = {
    { "file_inherit", VULTURE_ACL_FILE_INHERIT },
    { "directory_inherit", VULTURE_ACL_DIRECTORY_INHERIT },
    { "limit_inherit", VULTURE_ACL_LIMIT_INHERIT },
    { "only_inherit", VULTURE_ACL_ONLY_INHERIT },
};

inline void print_who(vulture_acl_entry const& entry)
{
    if (entry.tag == VULTURE_ACL_EVERYONE) {
        printf("group:everyone");
    } else if (entry.tag == VULTURE_ACL_USER) {
        auto const* user = getpwuid(entry.id);
        if (user)
            printf("user:%s", user->pw_name);
        else
            printf("user:%u", entry.id);
    } else {
        auto const* group = getgrgid(entry.id);
        if (group)
            printf("group:%s", group->gr_name);
        else
            printf("group:%u", entry.id);
    }
}

// As ls -e does: " 0: group:everyone deny write".
inline void print_line(size_t index, vulture_acl_entry const& entry, bool is_directory)
{
    printf(" %zu: ", index);
    print_who(entry);
    printf("%s %s ", (entry.inheritance & VULTURE_ACL_INHERITED) ? " inherited" : "", entry.kind == VULTURE_ACL_DENY ? "deny" : "allow");
    bool first = true;
    auto const say = [&](Word const& word) {
        printf("%s%s", first ? "" : ",", word.name);
        first = false;
    };
    if (is_directory) {
        for (auto const& word : directory_rights) {
            if (entry.permissions & word.value)
                say(word);
        }
    } else {
        for (auto const& word : file_rights) {
            if (entry.permissions & word.value)
                say(word);
        }
    }
    for (auto const& word : common_rights) {
        if (entry.permissions & word.value)
            say(word);
    }
    for (auto const& word : inheritance_words) {
        // (For a file, there's only this: nothing inherits from it.)
        if ((entry.inheritance & word.value) && (is_directory || word.value == VULTURE_ACL_LIMIT_INHERIT))
            say(word);
    }
    putchar('\n');
}

}
