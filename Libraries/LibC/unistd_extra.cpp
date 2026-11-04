/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <sys/resource.h>
#include <unistd.h>

using System::Syscall;

extern "C" {

extern char** environ;

static constexpr int max_exec_arguments = 256;

int execl(char const* path, char const* argument, ...)
{
    char* argv[max_exec_arguments];
    int count = 0;
    va_list arguments;
    va_start(arguments, argument);
    for (char const* next = argument; next && count < max_exec_arguments - 1; next = va_arg(arguments, char const*))
        argv[count++] = const_cast<char*>(next);
    va_end(arguments);
    argv[count] = nullptr;
    return execv(path, argv);
}

int execlp(char const* file, char const* argument, ...)
{
    char* argv[max_exec_arguments];
    int count = 0;
    va_list arguments;
    va_start(arguments, argument);
    for (char const* next = argument; next && count < max_exec_arguments - 1; next = va_arg(arguments, char const*))
        argv[count++] = const_cast<char*>(next);
    va_end(arguments);
    argv[count] = nullptr;
    return execvp(file, argv);
}

int execle(char const* path, char const* argument, ...)
{
    char* argv[max_exec_arguments];
    int count = 0;
    va_list arguments;
    va_start(arguments, argument);
    char const* next = argument;
    for (; next && count < max_exec_arguments - 1; next = va_arg(arguments, char const*))
        argv[count++] = const_cast<char*>(next);
    argv[count] = nullptr;
    char* const* envp = va_arg(arguments, char* const*);
    va_end(arguments);
    return execve(path, argv, envp);
}

int execvpe(char const* file, char* const argv[], char* const envp[])
{
    if (strchr(file, '/'))
        return execve(file, argv, envp);
    char const* search = getenv("PATH");
    if (!search)
        search = "/bin:/usr/bin:/sbin:/usr/sbin";
    int saved_error = ENOENT;
    char path[PATH_MAX];
    for (char const* cursor = search;;) {
        char const* end = strchr(cursor, ':');
        size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
        if (length == 0) {
            path[0] = '.';
            length = 1;
        } else if (length < sizeof(path)) {
            memcpy(path, cursor, length);
        }
        if (length + 1 + strlen(file) < sizeof(path)) {
            path[length] = '/';
            strcpy(path + length + 1, file);
            execve(path, argv, envp);
            if (errno != ENOENT)
                saved_error = errno;
        }
        if (!end)
            break;
        cursor = end + 1;
    }
    errno = saved_error;
    return -1;
}

long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:
        return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        int count = 1;
        size_t length = sizeof(count);
        sysctlbyname("hw.ncpu", &count, &length, nullptr, 0);
        return count;
    }
    case _SC_OPEN_MAX: {
        rlimit limit {};
        getrlimit(RLIMIT_NOFILE, &limit);
        return static_cast<long>(limit.rlim_cur);
    }
    case _SC_CLK_TCK:
        return 100;
    case _SC_ARG_MAX:
        return 256 * 1024;
    case _SC_CHILD_MAX:
        return 32768;
    case _SC_HOST_NAME_MAX:
        return 64;
    case _SC_LOGIN_NAME_MAX:
        return 64;
    case _SC_LINE_MAX:
        return 2048;
    case _SC_PHYS_PAGES: {
        unsigned long long bytes = 0;
        size_t length = sizeof(bytes);
        sysctlbyname("hw.memsize", &bytes, &length, nullptr, 0);
        return static_cast<long>(bytes / 4096);
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

long fpathconf(int, int name)
{
    switch (name) {
    case _PC_NAME_MAX:
        return NAME_MAX;
    case _PC_PATH_MAX:
        return PATH_MAX;
    case _PC_PIPE_BUF:
        return 4096;
    case _PC_LINK_MAX:
        return 65000;
    default:
        errno = EINVAL;
        return -1;
    }
}

long pathconf(char const*, int name)
{
    return fpathconf(-1, name);
}

int getpagesize(void)
{
    return 4096;
}

char* getlogin(void)
{
    static char name[64];
    if (char const* user = getenv("USER")) {
        strlcpy(name, user, sizeof(name));
        return name;
    }
    struct passwd* entry = getpwuid(getuid());
    if (!entry)
        return nullptr;
    strlcpy(name, entry->pw_name, sizeof(name));
    return name;
}

int getlogin_r(char* buffer, size_t size)
{
    char const* name = getlogin();
    if (!name)
        return ENOENT;
    return strlcpy(buffer, name, size) >= size ? ERANGE : 0;
}

// Reads a line from the terminal with echo off.
char* getpass(char const* prompt)
{
    static char password[128];
    int fd = open("/dev/tty", O_RDWR);
    bool opened = fd >= 0;
    if (!opened)
        fd = STDIN_FILENO;
    struct termios original, quiet;
    bool terminal = tcgetattr(fd, &original) == 0;
    if (terminal) {
        quiet = original;
        quiet.c_lflag &= ~(ECHO | ECHOE | ECHOK);
        quiet.c_lflag |= ECHONL;
        tcsetattr(fd, TCSAFLUSH, &quiet);
    }
    (void)write(opened ? fd : STDERR_FILENO, prompt, strlen(prompt));
    size_t length = 0;
    char character;
    while (length + 1 < sizeof(password) && read(fd, &character, 1) == 1 && character != '\n')
        password[length++] = character;
    password[length] = '\0';
    if (terminal)
        tcsetattr(fd, TCSAFLUSH, &original);
    if (opened)
        close(fd);
    return password;
}

unsigned alarm(unsigned seconds)
{
    return static_cast<unsigned>(System::syscall(Syscall::alarm, static_cast<long>(seconds)));
}

int pause(void)
{
    sigset_t mask;
    sigprocmask(SIG_SETMASK, nullptr, &mask);
    return sigsuspend(&mask);
}

int sigsuspend(sigset_t const* mask)
{
    long result = System::syscall(Syscall::sigsuspend, reinterpret_cast<long>(mask));
    errno = static_cast<int>(-result);
    return -1;
}

int sigpending(sigset_t* set)
{
    long result = System::syscall(Syscall::sigpending, reinterpret_cast<long>(set));
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return 0;
}

int killpg(pid_t group, int signal)
{
    if (group <= 1) {
        errno = EINVAL;
        return -1;
    }
    return kill(-group, signal);
}

void psignal(int signal, char const* prefix)
{
    if (prefix && *prefix)
        fprintf(stderr, "%s: %s\n", prefix, strsignal(signal));
    else
        fprintf(stderr, "%s\n", strsignal(signal));
}

int daemon(int stay_in_directory, int keep_output)
{
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child > 0)
        _exit(0);
    if (setsid() < 0)
        return -1;
    if (!stay_in_directory)
        chdir("/");
    if (!keep_output) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
            if (null > 2)
                close(null);
        }
    }
    return 0;
}

int dup3(int fd, int new_fd, int flags)
{
    if (fd == new_fd || (flags & ~O_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    if (dup2(fd, new_fd) < 0)
        return -1;
    if ((flags & O_CLOEXEC) && fcntl(new_fd, F_SETFD, FD_CLOEXEC) < 0)
        return -1;
    return new_fd;
}

// The file's data and its size are all one flush here.
int fdatasync(int fd)
{
    return fsync(fd);
}

int truncate(char const* path, off_t length)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int result = ftruncate(fd, length);
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

int tcsendbreak(int fd, int)
{
    // A break means nothing to our terminals; but it must be one.
    if (!isatty(fd))
        return -1;
    return 0;
}

}
