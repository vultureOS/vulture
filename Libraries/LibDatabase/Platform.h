/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// What the database needs of the system. This builds without any system's headers, for the host as well as for vultureOS,
// so these are said here, with the numbers that differ between them.

extern "C" {
int open(char const* path, int flags, ...);
int close(int fd);
long pread(int fd, void* buffer, unsigned long count, long offset);
long pwrite(int fd, void const* buffer, unsigned long count, long offset);
long lseek(int fd, long offset, int whence);
int fsync(int fd);
int ftruncate(int fd, long length);
int getpid();
double strtod(char const* text, char** end);
#if defined(__linux__)
int* __errno_location();
#else
int* __error();
#endif
}

namespace Database::Platform {

#if defined(__APPLE__)
constexpr int open_read_write_create = 0x0002 | 0x0200 | 0x1000000;
#else
constexpr int open_read_write_create = 0x0002 | 0x0040 | 0x80000;
#endif
constexpr int seek_end = 2;
constexpr int error_interrupted = 4;
constexpr int error_no_memory = 12;

inline int last_error()
{
#if defined(__linux__)
    return *__errno_location();
#else
    return *__error();
#endif
}

}
