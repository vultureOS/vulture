/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

// The group database, /etc/group: name:password:gid:member,member...
struct group {
    char* gr_name;
    char* gr_passwd;
    gid_t gr_gid;
    char** gr_mem;
};

struct group* getgrnam(char const* name);
struct group* getgrgid(gid_t);
struct group* getgrent(void);
void setgrent(void);
void endgrent(void);
// The groups `user` belongs to, `group` first.
int getgrouplist(char const* user, gid_t group, gid_t* groups, int* count);
int initgroups(char const* user, gid_t group);

__END_DECLS
