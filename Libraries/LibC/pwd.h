/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

// The user database, /etc/passwd: name:password:uid:gid:gecos:home:shell.
// Password hashes are kept apart, in /etc/master.passwd (root only).
struct passwd {
    char* pw_name;
    char* pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char* pw_gecos;
    char* pw_dir;
    char* pw_shell;
};

struct passwd* getpwnam(char const* name);
struct passwd* getpwuid(uid_t);
int getpwnam_r(char const* name, struct passwd*, char* buffer, size_t size, struct passwd** result);
int getpwuid_r(uid_t, struct passwd*, char* buffer, size_t size, struct passwd** result);
struct passwd* getpwent(void);
void setpwent(void);
void endpwent(void);

__END_DECLS
