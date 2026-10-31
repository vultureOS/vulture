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

extern char** environ;

pid_t fork(void);
int execve(char const* path, char* const argv[], char* const envp[]);
int execv(char const* path, char* const argv[]);
int execvp(char const* file, char* const argv[]);
__noreturn void _exit(int status);

pid_t getpid(void);
pid_t getppid(void);
pid_t gettid(void);

ssize_t read(int fd, void* buffer, size_t count);
ssize_t write(int fd, void const* buffer, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int origin);
int isatty(int fd);

unsigned sleep(unsigned seconds);
int usleep(useconds_t microseconds);

__END_DECLS
