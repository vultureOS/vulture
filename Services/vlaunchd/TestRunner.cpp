/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestRunner.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/vulture.h>
#include <sys/wait.h>
#include <unistd.h>

// The test boots: with "utest", process 1 runs the userland test programs,
// each in a child it forks and execs, judged by how the child ended; with
// "crashtest" and "persisttest", the storage tests' writer. Then it powers
// the machine off.

namespace Launchd {

namespace {

struct Test {
    char const* path;
    // The child must exit with `exit_code`, or be killed by `signal`.
    int exit_code;
    int signal;
};

Test const s_tests[] = {
    { "/usr/tests/hello", 0, 0 },
    { "/usr/tests/libc", 0, 0 },
    { "/usr/tests/libc-more", 0, 0 },
    { "/usr/tests/malloc", 0, 0 },
    { "/usr/tests/fork", 0, 0 },
    { "/usr/tests/copy-on-write", 0, 0 },
    { "/usr/tests/exec", 0, 0 },
    { "/usr/tests/dynamic", 0, 0 },
    { "/usr/tests/memory", 0, 0 },
    { "/usr/tests/bad-pointers", 0, 0 },
    { "/usr/tests/signals", 0, 0 },
    { "/usr/tests/sockets", 0, 0 },
    { "/usr/tests/crypto", 0, 0 },
    { "/usr/tests/hardening", 0, 0 },
    { "/usr/tests/sandbox", 0, 0 },
    { "/usr/tests/privacy", 0, 0 },
    // Only with the boot word that makes it mean anything (Meta/hardening-test.sh): else it fails.
    { "/usr/tests/kernel-fault", 1, 0 },
    { "/usr/tests/files", 0, 0 },
    { "/usr/tests/filesystem", 0, 0 },
    { "/usr/tests/pipes", 0, 0 },
    { "/usr/tests/devices", 0, 0 },
    { "/usr/tests/time", 0, 0 },
    { "/usr/tests/volumes", 0, 0 },
    { "/usr/tests/mapped-files", 0, 0 },
    { "/usr/tests/terminals", 0, 0 },
    { "/usr/tests/permissions", 0, 0 },
    { "/usr/tests/shell", 0, 0 },
    { "/usr/tests/tools", 0, 0 },
    { "/usr/tests/interactive", 0, 0 },
    { "/usr/tests/accounts", 0, 0 },
    { "/usr/tests/eventloop", 0, 0 },
    { "/usr/tests/launchd", 0, 0 },
    { "/usr/tests/posix", 0, 0 },
    { "/usr/tests/graphics", 0, 0 },
    { "/usr/tests/windows", 0, 0 },
    { "/usr/tests/toolkit", 0, 0 },
    { "/usr/tests/threads", 0, 0 },
    { "/usr/tests/ports", 0, 0 },
    { "/usr/tests/bootstrap", 0, 0 },
    { "/usr/tests/services", 0, 0 },
    { "/usr/tests/many-processes", 0, 0 },
    { "/usr/tests/process-info", 0, 0 },
    // A crash must kill only the crashing process.
    { "/usr/tests/segfault", 0, SIGSEGV },
    { "/usr/tests/abort", 0, SIGABRT },
};

void describe(int status, char* buffer, size_t size)
{
    if (WIFEXITED(status))
        snprintf(buffer, size, "exited with %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(buffer, size, "killed by %s%s", strsignal(WTERMSIG(status)), WCOREDUMP(status) ? ", crash report written" : "");
    else
        snprintf(buffer, size, "status %#x", status);
}

bool run(Test const& test)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("vlaunchd: fork");
        return false;
    }
    if (pid == 0) {
        char* argv[] = { const_cast<char*>(test.path), nullptr };
        execv(test.path, argv);
        fprintf(stderr, "vlaunchd: cannot run %s: %s\n", test.path, strerror(errno));
        _exit(127);
    }
    // Any child: orphans the test left are handed to us, and reaped here
    // as process 1 must.
    int status = 0;
    for (;;) {
        int any_status = 0;
        pid_t result = waitpid(-1, &any_status, 0);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0) {
            perror("vlaunchd: waitpid");
            return false;
        }
        if (result == pid) {
            status = any_status;
            break;
        }
    }
    bool passed = test.signal ? (WIFSIGNALED(status) && WTERMSIG(status) == test.signal)
                              : (WIFEXITED(status) && WEXITSTATUS(status) == test.exit_code);
    char how[64];
    describe(status, how, sizeof(how));
    printf("vlaunchd: [%s] %s (%s)\n", passed ? " OK " : "FAIL", test.path, how);
    return passed;
}

}

int run_tests(char const* only)
{
    int passed = 0;
    int failed = 0;
    for (auto const& test : s_tests) {
        if (only && strcmp(strrchr(test.path, '/') + 1, only) != 0)
            continue;
        if (run(test))
            ++passed;
        else
            ++failed;
    }
    // Asked for one, and it isn't there: that is no success.
    if (only && passed + failed == 0) {
        printf("vlaunchd: no test called %s\n", only);
        ++failed;
    }
    printf("userland: %d passed, %d failed\n", passed, failed);
    printf("userland: %s\n", failed == 0 ? "PASS" : "FAIL");
    fflush(stdout);
    vulture_power_off();
    return failed == 0 ? 0 : 1;
}

// The storage tests (Meta/storage-test.sh): runs the crash writer, which
// writes until the power is cut (or, with --once, a little).
int run_storage_test(bool once)
{
    pid_t pid = fork();
    if (pid == 0) {
        char* argv[] = { const_cast<char*>("/usr/tests/crash-writer"), const_cast<char*>(once ? "--once" : nullptr), nullptr };
        execv(argv[0], argv);
        fprintf(stderr, "vlaunchd: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    char how[64];
    describe(status, how, sizeof(how));
    printf("vlaunchd: crash-writer %s\n", how);
    fflush(stdout);
    vulture_power_off();
    return 0;
}

}
