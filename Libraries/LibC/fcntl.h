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

int open(char const* path, int flags, ...);
int openat(int directory, char const* path, int flags, ...);
int fcntl(int fd, int command, ...);

__END_DECLS
