/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/files.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// A copy that has the same blocks as the original until one or the other changes.
int clonefile(char const* source, char const* destination, unsigned int flags);
int clonefileat(int source_directory, char const* source, int destination_directory, char const* destination, unsigned int flags);
int fclonefileat(int source_fd, int destination_directory, char const* destination, unsigned int flags);

__END_DECLS
