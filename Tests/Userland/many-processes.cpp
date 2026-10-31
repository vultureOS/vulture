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
    // Many processes at once, all on the run across the processors.
    constexpr int count = 64;
    pid_t children[count];
    for (int i = 0; i < count; ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) {
            unsigned long sum = 0;
            for (unsigned long n = 0; n < 200'000; ++n)
                sum += n * static_cast<unsigned long>(i);
            _exit(static_cast<int>((sum + static_cast<unsigned long>(i)) % 256 == 256 ? 255 : i));
        }
    }
    int seen = 0;
    for (int i = 0; i < count; ++i) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        CHECK(pid > 0);
        for (int j = 0; j < count; ++j) {
            if (children[j] == pid) {
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == j);
                ++seen;
            }
        }
    }
    CHECK(seen == count);

    // An orphan: its parent exits first, and init adopts it.
    pid_t middle = fork();
    if (middle == 0) {
        if (fork() == 0) {
            usleep(20'000);
            _exit(getppid() == 1 ? 0 : 1);
        }
        _exit(0);
    }
    int status;
    CHECK(waitpid(middle, &status, 0) == middle && WEXITSTATUS(status) == 0);
    CHECK(waitpid(-1, &status, 0) == -1 && errno == ECHILD);
    usleep(50'000);
    return finish();
}
