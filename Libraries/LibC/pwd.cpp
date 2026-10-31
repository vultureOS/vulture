/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

// Splits a line at ':' into at most `count` fields, in place.
size_t split_fields(char* line, char** fields, size_t count)
{
    size_t found = 0;
    fields[found++] = line;
    for (char* cursor = line; *cursor && found < count; ++cursor) {
        if (*cursor == ':') {
            *cursor = '\0';
            fields[found++] = cursor + 1;
        }
    }
    char* end = fields[found - 1] + strcspn(fields[found - 1], "\n");
    *end = '\0';
    return found;
}

struct PasswdState {
    FILE* file { nullptr };
    char line[1024];
    passwd entry;
};

PasswdState s_passwd;

bool parse_passwd(char* line, passwd& entry)
{
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
        return false;
    char* fields[7];
    if (split_fields(line, fields, 7) != 7)
        return false;
    entry.pw_name = fields[0];
    entry.pw_passwd = fields[1];
    entry.pw_uid = static_cast<uid_t>(strtoul(fields[2], nullptr, 10));
    entry.pw_gid = static_cast<gid_t>(strtoul(fields[3], nullptr, 10));
    entry.pw_gecos = fields[4];
    entry.pw_dir = fields[5];
    entry.pw_shell = fields[6];
    return true;
}

struct GroupState {
    FILE* file { nullptr };
    char line[4096];
    char* members[128];
    group entry;
};

GroupState s_group;

bool parse_group(char* line, group& entry, char** members, size_t capacity)
{
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
        return false;
    char* fields[4];
    if (split_fields(line, fields, 4) != 4)
        return false;
    entry.gr_name = fields[0];
    entry.gr_passwd = fields[1];
    entry.gr_gid = static_cast<gid_t>(strtoul(fields[2], nullptr, 10));
    size_t count = 0;
    char* state = nullptr;
    for (char* member = strtok_r(fields[3], ",", &state); member && count + 1 < capacity; member = strtok_r(nullptr, ",", &state))
        members[count++] = member;
    members[count] = nullptr;
    entry.gr_mem = members;
    return true;
}

}

extern "C" {

void setpwent(void)
{
    if (s_passwd.file)
        rewind(s_passwd.file);
}

void endpwent(void)
{
    if (s_passwd.file)
        fclose(s_passwd.file);
    s_passwd.file = nullptr;
}

struct passwd* getpwent(void)
{
    if (!s_passwd.file) {
        s_passwd.file = fopen("/etc/passwd", "r");
        if (!s_passwd.file)
            return nullptr;
    }
    while (fgets(s_passwd.line, sizeof(s_passwd.line), s_passwd.file)) {
        if (parse_passwd(s_passwd.line, s_passwd.entry))
            return &s_passwd.entry;
    }
    return nullptr;
}

struct passwd* getpwnam(char const* name)
{
    setpwent();
    struct passwd* entry;
    while ((entry = getpwent())) {
        if (strcmp(entry->pw_name, name) == 0)
            break;
    }
    endpwent();
    return entry;
}

struct passwd* getpwuid(uid_t uid)
{
    setpwent();
    struct passwd* entry;
    while ((entry = getpwent())) {
        if (entry->pw_uid == uid)
            break;
    }
    endpwent();
    return entry;
}

// The _r forms copy the found entry's strings into `buffer`.
static int copy_passwd(struct passwd* found, struct passwd* entry, char* buffer, size_t size, struct passwd** result)
{
    *result = nullptr;
    if (!found)
        return 0;
    size_t used = 0;
    auto copy = [&](char const* text) -> char* {
        size_t length = strlen(text) + 1;
        if (used + length > size)
            return nullptr;
        char* place = buffer + used;
        memcpy(place, text, length);
        used += length;
        return place;
    };
    *entry = *found;
    entry->pw_name = copy(found->pw_name);
    entry->pw_passwd = copy(found->pw_passwd);
    entry->pw_gecos = copy(found->pw_gecos);
    entry->pw_dir = copy(found->pw_dir);
    entry->pw_shell = copy(found->pw_shell);
    if (!entry->pw_name || !entry->pw_passwd || !entry->pw_gecos || !entry->pw_dir || !entry->pw_shell)
        return ERANGE;
    *result = entry;
    return 0;
}

int getpwnam_r(char const* name, struct passwd* entry, char* buffer, size_t size, struct passwd** result)
{
    return copy_passwd(getpwnam(name), entry, buffer, size, result);
}

int getpwuid_r(uid_t uid, struct passwd* entry, char* buffer, size_t size, struct passwd** result)
{
    return copy_passwd(getpwuid(uid), entry, buffer, size, result);
}

void setgrent(void)
{
    if (s_group.file)
        rewind(s_group.file);
}

void endgrent(void)
{
    if (s_group.file)
        fclose(s_group.file);
    s_group.file = nullptr;
}

struct group* getgrent(void)
{
    if (!s_group.file) {
        s_group.file = fopen("/etc/group", "r");
        if (!s_group.file)
            return nullptr;
    }
    while (fgets(s_group.line, sizeof(s_group.line), s_group.file)) {
        if (parse_group(s_group.line, s_group.entry, s_group.members, sizeof(s_group.members) / sizeof(s_group.members[0])))
            return &s_group.entry;
    }
    return nullptr;
}

struct group* getgrnam(char const* name)
{
    setgrent();
    struct group* entry;
    while ((entry = getgrent())) {
        if (strcmp(entry->gr_name, name) == 0)
            break;
    }
    endgrent();
    return entry;
}

struct group* getgrgid(gid_t gid)
{
    setgrent();
    struct group* entry;
    while ((entry = getgrent())) {
        if (entry->gr_gid == gid)
            break;
    }
    endgrent();
    return entry;
}

int getgrouplist(char const* user, gid_t group_id, gid_t* groups, int* count)
{
    int capacity = *count;
    int found = 0;
    auto add = [&](gid_t id) {
        for (int i = 0; i < found; ++i) {
            if (i < capacity && groups[i] == id)
                return;
        }
        if (found < capacity)
            groups[found] = id;
        ++found;
    };
    add(group_id);
    setgrent();
    while (struct group* entry = getgrent()) {
        for (char** member = entry->gr_mem; *member; ++member) {
            if (strcmp(*member, user) == 0) {
                add(entry->gr_gid);
                break;
            }
        }
    }
    endgrent();
    *count = found;
    return found > capacity ? -1 : found;
}

int initgroups(char const* user, gid_t group_id)
{
    gid_t groups[16];
    int count = 16;
    if (getgrouplist(user, group_id, groups, &count) < 0)
        count = 16;
    return setgroups(static_cast<size_t>(count), groups);
}
}
