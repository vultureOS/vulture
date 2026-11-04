/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The performance budgets that take a running system to measure (see Meta/budgets.sh and Documentation/Roadmap.md), each
// said as "budget: NAME: VALUE UNIT". This only measures: whether that's good enough is for whoever reads it.

namespace {

double now_microseconds()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * 1'000'000.0 + static_cast<double>(now.tv_nsec) / 1'000.0;
}

void measure_null_syscall()
{
    // The cheapest thing there is to ask for, that can't be answered without asking. The best of several, as a hypervisor's
    // other work is no part of what this is.
    constexpr int calls = 1'000'000;
    double best = 0;
    for (int run = 0; run < 5; ++run) {
        double const started = now_microseconds();
        for (int i = 0; i < calls; ++i)
            (void)getppid();
        double const per_call = (now_microseconds() - started) * 1'000.0 / calls;
        if (run == 0 || per_call < best)
            best = per_call;
    }
    printf("budget: null syscall: %.0f ns\n", best);
}

// From being told to run it, until its first window has been put on the screen: what it says when
// VULTURE_REPORT_LAUNCH is set (see LibUI's Application).
void measure_launch(char const* budget, char const* program)
{
    int output[2];
    if (pipe(output) != 0) {
        printf("budget: %s: failed\n", budget);
        return;
    }
    double const started = now_microseconds();
    pid_t child = fork();
    if (child == 0) {
        dup2(output[1], STDOUT_FILENO);
        close(output[0]);
        close(output[1]);
        setenv("VULTURE_REPORT_LAUNCH", "1", 1);
        // Nothing moves or blinks: it's the launch being measured.
        setenv("VULTURE_STILL", "1", 1);
        execl(program, program, nullptr);
        _exit(127);
    }
    close(output[1]);
    fcntl(output[0], F_SETFL, O_NONBLOCK);
    char said[4096];
    size_t length = 0;
    double finished = 0;
    // Half a minute's plenty for anything.
    while (now_microseconds() - started < 30'000'000.0 && finished == 0) {
        ssize_t const got = read(output[0], said + length, sizeof(said) - length - 1);
        if (got > 0) {
            length += static_cast<size_t>(got);
            said[length] = 0;
            if (char const* line = strstr(said, "launch: first window on screen at ")) {
                if (strchr(line, '\n')) {
                    double const seconds = atof(line + strlen("launch: first window on screen at "));
                    finished = seconds * 1'000'000.0;
                }
            }
        } else if (got == 0) {
            break;
        } else {
            usleep(1'000);
        }
    }
    kill(child, SIGTERM);
    int status = 0;
    (void)waitpid(child, &status, 0);
    close(output[0]);
    if (finished == 0) {
        printf("budget: %s: failed\n", budget);
        return;
    }
    printf("budget: %s: %.0f ms\n", budget, (finished - started) / 1'000.0);
}

}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // The window server first, and everything it starts settled down.
    usleep(2'000'000);
    measure_null_syscall();
    fflush(stdout);
    measure_launch("Text Editor cold launch", "/System/Applications/Text Editor.app/Contents/MacOS/TextEditor");
    usleep(500'000);
    measure_launch("Calculator cold launch", "/System/Applications/Calculator.app/Contents/MacOS/Calculator");
    usleep(500'000);
    measure_launch("Text Editor warm launch", "/System/Applications/Text Editor.app/Contents/MacOS/TextEditor");
    usleep(500'000);

    // The window server's own numbers, dragging a window across others (Tests/Userland/windows.cpp).
    pid_t drag = fork();
    if (drag == 0) {
        execl("/usr/tests/windows", "/usr/tests/windows", "--budget", nullptr);
        _exit(127);
    }
    int status = 0;
    (void)waitpid(drag, &status, 0);
    printf("budgets: done\n");
    fflush(stdout);
    for (;;)
        pause();
}
