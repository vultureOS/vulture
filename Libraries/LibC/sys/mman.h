/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/memory.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define MAP_FAILED VULTURE_MAP_FAILED

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void* address, size_t length);
int mprotect(void* address, size_t length, int protection);

__END_DECLS
