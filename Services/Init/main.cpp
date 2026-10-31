/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/vulture.h>
#include <sys/wait.h>
#include <unistd.h>

// Process 1. For now it greets, and with "--test" runs the userland test
// programs: each in a child it forks and execs, judged by how the child
// ended. It also reaps orphans, which the kernel hands to it. (vlaunchd
// takes over this job in M6.)

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
    { "/usr/tests/malloc", 0, 0 },
    { "/usr/tests/fork", 0, 0 },
    { "/usr/tests/copy-on-write", 0, 0 },
    { "/usr/tests/exec", 0, 0 },
    { "/usr/tests/memory", 0, 0 },
    { "/usr/tests/bad-pointers", 0, 0 },
    { "/usr/tests/signals", 0, 0 },
    { "/usr/tests/files", 0, 0 },
    { "/usr/tests/filesystem", 0, 0 },
    { "/usr/tests/pipes", 0, 0 },
    { "/usr/tests/devices", 0, 0 },
    { "/usr/tests/time", 0, 0 },
    { "/usr/tests/many-processes", 0, 0 },
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
        perror("init: fork");
        return false;
    }
    if (pid == 0) {
        char* argv[] = { const_cast<char*>(test.path), nullptr };
        execv(test.path, argv);
        fprintf(stderr, "init: cannot run %s: %s\n", test.path, strerror(errno));
        _exit(127);
    }
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != pid) {
        perror("init: waitpid");
        return false;
    }
    bool passed = test.signal ? (WIFSIGNALED(status) && WTERMSIG(status) == test.signal)
                              : (WIFEXITED(status) && WEXITSTATUS(status) == test.exit_code);
    char how[64];
    describe(status, how, sizeof(how));
    printf("init: [%s] %s (%s)\n", passed ? " OK " : "FAIL", test.path, how);
    return passed;
}

int run_tests()
{
    int passed = 0;
    int failed = 0;
    for (auto const& test : s_tests) {
        if (run(test))
            ++passed;
        else
            ++failed;
    }
    printf("userland: %d passed, %d failed\n", passed, failed);
    printf("userland: %s\n", failed == 0 ? "PASS" : "FAIL");
    fflush(stdout);
    vulture_power_off();
    return failed == 0 ? 0 : 1;
}

void print_file(char const* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    char buffer[256];
    ssize_t length;
    while ((length = read(fd, buffer, sizeof(buffer))) > 0)
        write(STDOUT_FILENO, buffer, static_cast<size_t>(length));
    close(fd);
}

}

int main(int argc, char** argv)
{
    printf("init: vultureOS userland is up (process %d)\n", getpid());
    print_file("/etc/motd");

    if (argc > 1 && strcmp(argv[1], "--test") == 0)
        return run_tests();

    // Reap whatever the kernel hands over; sleep while there is nothing.
    for (;;) {
        int status;
        if (waitpid(-1, &status, 0) < 0 && errno == ECHILD)
            sleep(3600);
        // EINTR: a signal; just wait again.
    }
}
