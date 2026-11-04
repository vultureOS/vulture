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

// As a Mac has them. `position` is only for what's not here (a resource fork): it has to be 0.
ssize_t getxattr(char const* path, char const* name, void* value, size_t size, unsigned int position, int options);
ssize_t fgetxattr(int fd, char const* name, void* value, size_t size, unsigned int position, int options);
int setxattr(char const* path, char const* name, void const* value, size_t size, unsigned int position, int options);
int fsetxattr(int fd, char const* name, void const* value, size_t size, unsigned int position, int options);
int removexattr(char const* path, char const* name, int options);
int fremovexattr(int fd, char const* name, int options);
ssize_t listxattr(char const* path, char* names, size_t size, int options);
ssize_t flistxattr(int fd, char* names, size_t size, int options);

__END_DECLS
