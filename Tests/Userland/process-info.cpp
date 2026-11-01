/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

// Asking about processes while they come and go: "kern.proc.all" looks into every
// process's address space, and exec and exit are what free those. (logd asks for every
// message it files, so this goes on all the time.)

namespace {

constexpr int readers = 6;
constexpr int rounds = 3000;

[[noreturn]] void keep_asking()
{
    static char buffer[256 * 1024];
    for (;;) {
        size_t length = sizeof(buffer);
        if (sysctlbyname("kern.proc.all", buffer, &length, nullptr, 0) != 0)
            _exit(2);
    }
}

}

int main()
{
    pid_t asking[readers];
    for (auto& reader : asking) {
        reader = fork();
        CHECK(reader >= 0);
        if (reader == 0)
            keep_asking();
    }

    for (int round = 0; round < rounds; ++round) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            // Half go by exec, which swaps the address space for another; half by exit,
            // which frees it with the last thread.
            if (round % 2)
                execl("/bin/true", "true", nullptr);
            _exit(0);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    for (auto reader : asking) {
        kill(reader, SIGKILL);
        int status = 0;
        CHECK(waitpid(reader, &status, 0) == reader);
        // Killed: if it left by itself, asking failed.
        CHECK(WIFSIGNALED(status));
    }
    return finish();
}
