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
// Password hashes are kept apart, in /etc/master.passwd (readable by root
// only), as on BSD: name:password:uid:gid:class:change:expire:gecos:home:
// shell. For root, the functions here read that file, so pw_passwd is the
// hash; for everyone else it is "*".
struct passwd {
    char* pw_name;
    char* pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    time_t pw_change; // When the password must change; 0: never.
    char* pw_class;
    char* pw_gecos;
    char* pw_dir;
    char* pw_shell;
    time_t pw_expire; // When the account expires; 0: never.
};

struct passwd* getpwnam(char const* name);
struct passwd* getpwuid(uid_t);
int getpwnam_r(char const* name, struct passwd*, char* buffer, size_t size, struct passwd** result);
int getpwuid_r(uid_t, struct passwd*, char* buffer, size_t size, struct passwd** result);
struct passwd* getpwent(void);
void setpwent(void);
void endpwent(void);

__END_DECLS
