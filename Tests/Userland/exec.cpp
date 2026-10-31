/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

int main()
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        char* argv[] = { const_cast<char*>("exec-target"), const_cast<char*>("one"), const_cast<char*>("two"), nullptr };
        char* envp[] = { const_cast<char*>("PATH=/bin"), const_cast<char*>("VULTURE_TEST=yes"), nullptr };
        execve("/usr/tests/exec-target", argv, envp);
        _exit(100);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);

    // Failures leave the caller running, with errno set.
    char* argv[] = { const_cast<char*>("missing"), nullptr };
    CHECK(execv("/usr/tests/does-not-exist", argv) == -1 && errno == ENOENT);
    // Not executable (and not a program either).
    CHECK(execv("/etc/motd", argv) == -1 && errno == EACCES);
    CHECK(execv("/etc", argv) == -1 && errno == EACCES);
    return finish();
}
