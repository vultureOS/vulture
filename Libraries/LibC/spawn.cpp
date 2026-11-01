/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct __posix_spawn_action {
    enum class Kind {
        Open,
        Close,
        Dup2,
    } kind;
    int fd;
    int other_fd; // Dup2: the source.
    char* path;   // Open.
    int flags;
    mode_t mode;
};

namespace {

int add_action(posix_spawn_file_actions_t* actions, __posix_spawn_action const& action)
{
    if (!actions)
        return EINVAL;
    if (action.fd < 0 || action.fd >= OPEN_MAX || (action.kind == __posix_spawn_action::Kind::Dup2 && (action.other_fd < 0 || action.other_fd >= OPEN_MAX)))
        return EBADF;
    if (actions->count == actions->capacity) {
        int capacity = actions->capacity ? actions->capacity * 2 : 8;
        auto* grown = static_cast<__posix_spawn_action*>(realloc(actions->actions, static_cast<size_t>(capacity) * sizeof(__posix_spawn_action)));
        if (!grown)
            return ENOMEM;
        actions->actions = grown;
        actions->capacity = capacity;
    }
    actions->actions[actions->count++] = action;
    return 0;
}

// In the child: the set-up, then the program; on any failure, the error
// number goes back through `report` and the child ends.
[[noreturn]] void become(char const* path, bool search, posix_spawn_file_actions_t const* actions, posix_spawnattr_t const* attributes, char* const argv[],
    char* const envp[], int report)
{
    auto fail = [report](int error) {
        (void)write(report, &error, sizeof(error));
        _exit(127);
    };
    if (attributes) {
        short flags = attributes->flags;
        if ((flags & POSIX_SPAWN_SETSID) && setsid() < 0)
            fail(errno);
        if ((flags & POSIX_SPAWN_SETPGROUP) && setpgid(0, attributes->process_group) < 0)
            fail(errno);
        if (flags & POSIX_SPAWN_SETSIGDEF) {
            for (int signal = 1; signal < NSIG; ++signal) {
                if (sigismember(&attributes->defaults, signal))
                    ::signal(signal, SIG_DFL);
            }
        }
        if ((flags & POSIX_SPAWN_SETSIGMASK) && sigprocmask(SIG_SETMASK, &attributes->mask, nullptr) < 0)
            fail(errno);
        if (flags & POSIX_SPAWN_RESETIDS) {
            if (setegid(getgid()) < 0 || seteuid(getuid()) < 0)
                fail(errno);
        }
    }
    if (actions) {
        for (int i = 0; i < actions->count; ++i) {
            auto const& action = actions->actions[i];
            switch (action.kind) {
            case __posix_spawn_action::Kind::Open: {
                int fd = open(action.path, action.flags, action.mode);
                if (fd < 0)
                    fail(errno);
                if (fd != action.fd) {
                    if (dup2(fd, action.fd) < 0)
                        fail(errno);
                    close(fd);
                }
                break;
            }
            case __posix_spawn_action::Kind::Close:
                close(action.fd);
                break;
            case __posix_spawn_action::Kind::Dup2:
                if (action.other_fd == action.fd) {
                    // Kept across exec, as POSIX says of a dup2 onto itself.
                    if (fcntl(action.fd, F_SETFD, 0) < 0)
                        fail(errno);
                } else if (dup2(action.other_fd, action.fd) < 0) {
                    fail(errno);
                }
                break;
            }
        }
    }
    if (!search || strchr(path, '/')) {
        execve(path, argv, envp);
        fail(errno);
    }
    // PATH, from our environment; the first that runs.
    char const* directories = getenv("PATH");
    if (!directories || !*directories)
        directories = "/usr/bin:/bin:/usr/sbin:/sbin";
    int error = ENOENT;
    for (char const* cursor = directories;;) {
        char const* end = strchr(cursor, ':');
        size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
        char candidate[PATH_MAX];
        if (length + strlen(path) + 2 <= sizeof(candidate)) {
            memcpy(candidate, length ? cursor : ".", length ? length : 1);
            size_t at = length ? length : 1;
            candidate[at++] = '/';
            strcpy(candidate + at, path);
            execve(candidate, argv, envp);
            if (errno != ENOENT)
                error = errno;
        }
        if (!end)
            break;
        cursor = end + 1;
    }
    fail(error);
    __builtin_unreachable();
}

int spawn(pid_t* pid, char const* path, bool search, posix_spawn_file_actions_t const* actions, posix_spawnattr_t const* attributes, char* const argv[],
    char* const envp[])
{
    int report[2];
    if (pipe2(report, O_CLOEXEC) < 0)
        return errno;
    pid_t child = fork();
    if (child < 0) {
        int error = errno;
        close(report[0]);
        close(report[1]);
        return error;
    }
    if (child == 0) {
        close(report[0]);
        become(path, search, actions, attributes, argv, envp, report[1]);
    }
    close(report[1]);
    // Nothing comes back when the program started (the pipe closed on exec).
    int error = 0;
    ssize_t got;
    do {
        got = read(report[0], &error, sizeof(error));
    } while (got < 0 && errno == EINTR);
    close(report[0]);
    if (got == sizeof(error)) {
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        return error;
    }
    if (pid)
        *pid = child;
    return 0;
}

}

extern "C" {

int posix_spawn(pid_t* pid, char const* path, posix_spawn_file_actions_t const* actions, posix_spawnattr_t const* attributes, char* const argv[], char* const envp[])
{
    return spawn(pid, path, false, actions, attributes, argv, envp);
}

int posix_spawnp(pid_t* pid, char const* file, posix_spawn_file_actions_t const* actions, posix_spawnattr_t const* attributes, char* const argv[], char* const envp[])
{
    return spawn(pid, file, true, actions, attributes, argv, envp);
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* actions)
{
    *actions = {};
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* actions)
{
    for (int i = 0; i < actions->count; ++i)
        free(actions->actions[i].path);
    free(actions->actions);
    *actions = {};
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* actions, int fd, char const* path, int flags, mode_t mode)
{
    char* copy = strdup(path);
    if (!copy)
        return ENOMEM;
    int error = add_action(actions, { __posix_spawn_action::Kind::Open, fd, -1, copy, flags, mode });
    if (error)
        free(copy);
    return error;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* actions, int fd)
{
    return add_action(actions, { __posix_spawn_action::Kind::Close, fd, -1, nullptr, 0, 0 });
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* actions, int fd, int new_fd)
{
    return add_action(actions, { __posix_spawn_action::Kind::Dup2, new_fd, fd, nullptr, 0, 0 });
}

int posix_spawnattr_init(posix_spawnattr_t* attributes)
{
    *attributes = {};
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t*)
{
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t* attributes, short flags)
{
    if (flags & ~(POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSID))
        return EINVAL;
    attributes->flags = flags;
    return 0;
}

int posix_spawnattr_getflags(posix_spawnattr_t const* attributes, short* flags)
{
    *flags = attributes->flags;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t* attributes, pid_t group)
{
    attributes->process_group = group;
    return 0;
}

int posix_spawnattr_getpgroup(posix_spawnattr_t const* attributes, pid_t* group)
{
    *group = attributes->process_group;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t* attributes, sigset_t const* mask)
{
    attributes->mask = *mask;
    return 0;
}

int posix_spawnattr_getsigmask(posix_spawnattr_t const* attributes, sigset_t* mask)
{
    *mask = attributes->mask;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t* attributes, sigset_t const* defaults)
{
    attributes->defaults = *defaults;
    return 0;
}

int posix_spawnattr_getsigdefault(posix_spawnattr_t const* attributes, sigset_t* defaults)
{
    *defaults = attributes->defaults;
    return 0;
}

}
