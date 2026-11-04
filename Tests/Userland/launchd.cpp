/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// vlaunchd's jobs, with vlaunchd supervising a directory of property lists
// (as it does /System/Library/LaunchDaemons as process 1): RunAtLoad,
// KeepAlive (always, after success, after failure), ThrottleInterval,
// StartInterval, the environment, working directory, output and user,
// Disabled, a broken plist, a program that is not there, and stopping
// everything (SIGKILL for a job that will not stop).

namespace {

char const* const s_directory = "/tmp/launchd-test";

void pause_milliseconds(long milliseconds)
{
    struct timespec duration { milliseconds / 1000, (milliseconds % 1000) * 1'000'000 };
    nanosleep(&duration, nullptr);
}

void write_file(char const* name, char const* text)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == static_cast<ssize_t>(strlen(text)));
    close(fd);
}

// The file's contents ("" if it is not there).
char const* read_file(char const* name)
{
    static char text[8192];
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    text[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return text;
    ssize_t got = read(fd, text, sizeof(text) - 1);
    text[got > 0 ? got : 0] = '\0';
    close(fd);
    return text;
}

int count_lines(char const* name)
{
    int lines = 0;
    for (char const* c = read_file(name); *c; ++c)
        lines += *c == '\n';
    return lines;
}

bool file_exists(char const* name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    return access(path, F_OK) == 0;
}

void expect_lines(char const* name, int low, int high)
{
    int lines = count_lines(name);
    if (lines < low || lines > high)
        fprintf(stderr, "launchd: %s has %d lines, not %d to %d\n", name, lines, low, high);
    CHECK(lines >= low && lines <= high);
}

void expect_in_log(char const* text)
{
    bool found = strstr(read_file("log"), text) != nullptr;
    if (!found)
        fprintf(stderr, "launchd: the log lacks \"%s\"; it says:\n%s", text, read_file("log"));
    CHECK(found);
}

}

int main()
{
    system("rm -rf /tmp/launchd-test");
    CHECK(mkdir(s_directory, 0755) == 0);

    write_file("once.plist", R"({
        "Label" : "com.test.once",
        "ProgramArguments" : [ "/bin/sh", "-c", "echo ran >> /tmp/launchd-test/once.out" ],
        "RunAtLoad" : true
    })");
    write_file("keep.plist", R"({
        "Label" : "com.test.keep",
        "ProgramArguments" : [ "/bin/sh", "-c", "echo $$ >> /tmp/launchd-test/keep.out; sleep 0.3" ],
        "KeepAlive" : true,
        "ThrottleInterval" : 0
    })");
    write_file("throttled.plist", R"({
        "Label" : "com.test.throttled",
        "ProgramArguments" : [ "/bin/sh", "-c", "echo run >> /tmp/launchd-test/throttled.out" ],
        "KeepAlive" : true,
        "ThrottleInterval" : 2
    })");
    write_file("failing.plist", R"({
        "Label" : "com.test.failing",
        "ProgramArguments" : [ "/bin/sh", "-c", "n=$(grep -c run /tmp/launchd-test/failing.out 2>/dev/null); echo run >> /tmp/launchd-test/failing.out; [ \"${n:-0}\" -ge 2 ]" ],
        "KeepAlive" : { "SuccessfulExit" : false },
        "ThrottleInterval" : 0
    })");
    write_file("success.plist", R"({
        "Label" : "com.test.success",
        "ProgramArguments" : [ "/bin/sh", "-c", "echo run >> /tmp/launchd-test/success.out; exit 1" ],
        "RunAtLoad" : true,
        "KeepAlive" : { "SuccessfulExit" : true },
        "ThrottleInterval" : 0
    })");
    write_file("interval.plist", R"({
        "Label" : "com.test.interval",
        "ProgramArguments" : [ "/bin/sh", "-c", "echo tick >> /tmp/launchd-test/interval.out" ],
        "StartInterval" : 1
    })");
    write_file("environment.plist", R"({
        "Label" : "com.test.environment",
        "Program" : "/bin/sh",
        "ProgramArguments" : [ "sh", "-c", "echo \"$GREETING $(pwd) $PATH\"" ],
        "EnvironmentVariables" : { "GREETING" : "hello" },
        "WorkingDirectory" : "/tmp/launchd-test",
        "StandardOutPath" : "/tmp/launchd-test/environment.out",
        "RunAtLoad" : true
    })");
    write_file("user.plist", R"({
        "Label" : "com.test.user",
        "ProgramArguments" : [ "/bin/sh", "-c", "id -un; echo $HOME" ],
        "UserName" : "vulture",
        "StandardOutPath" : "/tmp/launchd-test/user.out",
        "RunAtLoad" : true
    })");
    write_file("disabled.plist", R"({
        "Label" : "com.test.disabled",
        "ProgramArguments" : [ "/bin/sh", "-c", "echo ran >> /tmp/launchd-test/disabled.out" ],
        "RunAtLoad" : true,
        "Disabled" : true
    })");
    write_file("missing.plist", R"({
        "Label" : "com.test.missing",
        "ProgramArguments" : [ "/nonexistent/program" ],
        "RunAtLoad" : true
    })");
    write_file("broken.plist", "{\n    \"Label\" : \"com.test.broken\",\n}\n");
    write_file("daemon.plist", R"({
        "Label" : "com.test.daemon",
        "ProgramArguments" : [ "/bin/sleep", "100" ],
        "KeepAlive" : true
    })");
    write_file("stubborn.plist", R"({
        "Label" : "com.test.stubborn",
        "ProgramArguments" : [ "/bin/sh", "-c", "trap '' TERM; echo up >> /tmp/launchd-test/stubborn.out; sleep 100" ],
        "KeepAlive" : true,
        "ExitTimeOut" : 1
    })");

    pid_t launchd = fork();
    if (launchd == 0) {
        char const* argv[] = { "vlaunchd", "--log", "/tmp/launchd-test/log", "--directory", s_directory, nullptr };
        execv("/sbin/vlaunchd", const_cast<char**>(argv));
        _exit(127);
    }
    CHECK(launchd > 0);
    pause_milliseconds(2500);

    // Stopping: everything is told to end, the stubborn job killed.
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    CHECK(kill(launchd, SIGTERM) == 0);
    int status = -1;
    for (int waited = 0; waited < 100 && waitpid(launchd, &status, WNOHANG) == 0; ++waited)
        pause_milliseconds(100);
    clock_gettime(CLOCK_MONOTONIC, &after);
    long took = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1'000'000;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (status == -1) {
        kill(launchd, SIGKILL);
        waitpid(launchd, &status, 0);
    }
    // One second for the stubborn job's ExitTimeOut, then SIGKILL.
    CHECK(took >= 900 && took < 5000);

    expect_lines("once.out", 1, 1);
    expect_lines("keep.out", 3, 20);
    expect_lines("throttled.out", 2, 2);
    expect_lines("failing.out", 3, 3);
    expect_lines("success.out", 1, 1);
    expect_lines("interval.out", 1, 3);
    expect_lines("stubborn.out", 1, 1);
    CHECK(!file_exists("disabled.out"));
    CHECK(strcmp(read_file("environment.out"), "hello /private/tmp/launchd-test /usr/bin:/bin:/usr/sbin:/sbin\n") == 0);
    CHECK(strcmp(read_file("user.out"), "vulture\n/Users/vulture\n") == 0);

    // KeepAlive started each run anew: different processes.
    char const* pids = read_file("keep.out");
    char first[16] = {};
    sscanf(pids, "%15s", first);
    CHECK(strstr(pids + strlen(first) + 1, first) == nullptr);

    expect_in_log("vlaunchd[");
    expect_in_log("(com.test.failing[");
    expect_in_log("Service exited with abnormal code: 1");
    expect_in_log("(com.test.throttled) Service only ran for 0 seconds. Pushing respawn out by 2 seconds.");
    expect_in_log("/tmp/launchd-test/broken.plist:3:1: expected a member name in quotes");
    expect_in_log("(com.test.missing) Could not run /nonexistent/program: No such file or directory");
    expect_in_log("(com.test.stubborn[");
    expect_in_log("Exited with SIGKILL after not stopping in time");
    CHECK(strstr(read_file("log"), "com.test.disabled") == nullptr);

    // Nothing left running.
    // (Zombies are only waiting for process 1 to reap them.)
    system("ps -Aj | grep ' sleep$' | grep -vc ' Z ' > /tmp/launchd-test/sleeping");
    bool none = strcmp(read_file("sleeping"), "0\n") == 0;
    if (!none) {
        system("ps -Aj > /tmp/launchd-test/processes");
        fprintf(stderr, "launchd: still running:\n%s", read_file("processes"));
    }
    CHECK(none);
    system("rm -rf /tmp/launchd-test");
    return finish();
}
