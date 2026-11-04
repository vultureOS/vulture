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

// Shared file mappings are the page cache itself: what is written is in
// the file at once. MS_SYNC also writes it to the disk.
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4
int msync(void* address, size_t length, int flags);

// Named shared memory: for mmap(MAP_SHARED), by a name any process can find.
int shm_open(char const* name, int flags, mode_t mode);
int shm_unlink(char const* name);

// Advice. MADV_DONTNEED and MADV_FREE give up the pages of private memory, which read as zeroes afterwards. The others are
// noted, and do nothing more for now.
#define MADV_FREE 5
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define POSIX_MADV_NORMAL MADV_NORMAL
#define POSIX_MADV_RANDOM MADV_RANDOM
#define POSIX_MADV_SEQUENTIAL MADV_SEQUENTIAL
#define POSIX_MADV_WILLNEED MADV_WILLNEED
#define POSIX_MADV_DONTNEED MADV_DONTNEED
int madvise(void* address, size_t length, int advice);
int posix_madvise(void* address, size_t length, int advice);

__END_DECLS
