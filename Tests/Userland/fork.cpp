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
    pid_t self = getpid();
    constexpr int count = 10;
    pid_t children[count];
    for (int i = 0; i < count; ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) {
            // In the child: a new process whose parent is us.
            if (getppid() != self || getpid() == self)
                _exit(200);
            _exit(i + 1);
        }
    }
    // Reap them out of order, by pid.
    for (int i = count; i-- > 0;) {
        int status = 0;
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == i + 1);
    }
    int status;
    CHECK(waitpid(-1, &status, 0) == -1 && errno == ECHILD);
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);

    // WNOHANG does not wait for a running child.
    pid_t sleeper = fork();
    if (sleeper == 0) {
        usleep(50'000);
        _exit(7);
    }
    CHECK(waitpid(sleeper, &status, WNOHANG) == 0);
    CHECK(waitpid(sleeper, &status, 0) == sleeper && WEXITSTATUS(status) == 7);
    return finish();
}
