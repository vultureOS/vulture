/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/files.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct DIR DIR;

DIR* opendir(char const* path);
DIR* fdopendir(int fd);
struct dirent* readdir(DIR*);
void rewinddir(DIR*);
int closedir(DIR*);
int dirfd(DIR*);

__END_DECLS
