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
int execvpe(char const* file, char* const argv[], char* const envp[]);
int execl(char const* path, char const* argument, ...);
int execlp(char const* file, char const* argument, ...);
int execle(char const* path, char const* argument, ...);
__noreturn void _exit(int status);

pid_t getpid(void);
pid_t getppid(void);
pid_t gettid(void);

// Users and groups.
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t);
int setgid(gid_t);
int seteuid(uid_t);
int setegid(gid_t);
int getgroups(int size, gid_t list[]);
int setgroups(size_t size, gid_t const* list);
int chown(char const* path, uid_t, gid_t);
int lchown(char const* path, uid_t, gid_t);
int fchown(int fd, uid_t, gid_t);
int link(char const* existing, char const* created);
int gethostname(char* name, size_t size);
int sethostname(char const* name, size_t size);

// Job control.
pid_t setsid(void);
pid_t getsid(pid_t);
int setpgid(pid_t, pid_t group);
pid_t getpgid(pid_t);
pid_t getpgrp(void);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t group);
char* ttyname(int fd);
int ttyname_r(int fd, char* buffer, size_t size);

ssize_t read(int fd, void* buffer, size_t count);
ssize_t write(int fd, void const* buffer, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int origin);
int isatty(int fd);
int dup(int fd);
int dup2(int fd, int new_fd);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int fsync(int fd);
int ftruncate(int fd, off_t length);
void sync(void);

int unlink(char const* path);
int rmdir(char const* path);
int chdir(char const* path);
char* getcwd(char* buffer, size_t size);
ssize_t readlink(char const* path, char* buffer, size_t size);
int symlink(char const* target, char const* link_path);
int access(char const* path, int mode);

unsigned sleep(unsigned seconds);
unsigned alarm(unsigned seconds);
int pause(void);
int daemon(int stay_in_directory, int keep_output);
char* getlogin(void);
int getlogin_r(char* buffer, size_t size);
char* getpass(char const* prompt);
int getpagesize(void);

// getopt() (see getopt.h for getopt_long).
extern char* optarg;
extern int optind;
extern int opterr;
extern int optopt;
int getopt(int argc, char* const argv[], char const* options);

#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _SC_LINE_MAX 43
#define _SC_HOST_NAME_MAX 180
#define _SC_LOGIN_NAME_MAX 71
long sysconf(int name);

#define _PC_LINK_MAX 0
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5
long pathconf(char const* path, int name);
long fpathconf(int fd, int name);

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
int usleep(useconds_t microseconds);

__END_DECLS
