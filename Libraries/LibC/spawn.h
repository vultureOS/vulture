/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <signal.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

// posix_spawn: a new process running a program, set up as the file
// actions and attributes say (in one call, as macOS programs start most
// others). The functions return an error number, not -1.

#define POSIX_SPAWN_RESETIDS 0x01
#define POSIX_SPAWN_SETPGROUP 0x02
#define POSIX_SPAWN_SETSIGDEF 0x04
#define POSIX_SPAWN_SETSIGMASK 0x08
#define POSIX_SPAWN_SETSID 0x400

typedef struct {
    short flags;
    pid_t process_group;
    sigset_t mask;
    sigset_t defaults;
} posix_spawnattr_t;

struct __posix_spawn_action;
typedef struct {
    int count;
    int capacity;
    struct __posix_spawn_action* actions;
} posix_spawn_file_actions_t;

int posix_spawn(pid_t* pid, char const* path, posix_spawn_file_actions_t const*, posix_spawnattr_t const*, char* const argv[], char* const envp[]);
// As posix_spawn, looking for `file` along PATH when it has no slash.
int posix_spawnp(pid_t* pid, char const* file, posix_spawn_file_actions_t const*, posix_spawnattr_t const*, char* const argv[], char* const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t*);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int fd, char const* path, int flags, mode_t);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int fd, int new_fd);

int posix_spawnattr_init(posix_spawnattr_t*);
int posix_spawnattr_destroy(posix_spawnattr_t*);
int posix_spawnattr_setflags(posix_spawnattr_t*, short flags);
int posix_spawnattr_getflags(posix_spawnattr_t const*, short* flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t*, pid_t);
int posix_spawnattr_getpgroup(posix_spawnattr_t const*, pid_t*);
int posix_spawnattr_setsigmask(posix_spawnattr_t*, sigset_t const*);
int posix_spawnattr_getsigmask(posix_spawnattr_t const*, sigset_t*);
int posix_spawnattr_setsigdefault(posix_spawnattr_t*, sigset_t const*);
int posix_spawnattr_getsigdefault(posix_spawnattr_t const*, sigset_t*);

__END_DECLS
