/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// time command [argument...]: how long it took, as macOS's /usr/bin/time.
int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: time command [argument ...]\n");
        return 1;
    }
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pid_t child = fork();
    if (child == 0) {
        execvp(argv[1], argv + 1);
        fprintf(stderr, "time: %s: %s\n", argv[1], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    // The child's CPU time, read just before it is reaped.
    unsigned long long cpu = 0;
    int status = 0;
    for (;;) {
        size_t length = 0;
        if (sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0) == 0) {
            length += 8 * sizeof(vulture_process_info);
            auto* processes = static_cast<vulture_process_info*>(malloc(length));
            if (sysctlbyname("kern.proc.all", processes, &length, nullptr, 0) == 0) {
                for (size_t i = 0; i < length / sizeof(vulture_process_info); ++i) {
                    if (processes[i].pid == child)
                        cpu = processes[i].cpu_nanoseconds;
                }
            }
            free(processes);
        }
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child)
            break;
        struct timespec pause { 0, 5'000'000 };
        nanosleep(&pause, nullptr);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double real = static_cast<double>(end.tv_sec - start.tv_sec) + static_cast<double>(end.tv_nsec - start.tv_nsec) / 1e9;
    fprintf(stderr, "%10.2f real %10.2f user %10.2f sys\n", real, static_cast<double>(cpu) / 1e9, 0.0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
