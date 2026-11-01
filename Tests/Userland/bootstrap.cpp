/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <EchoEndpoint.h>
#include <LibIPC/Bootstrap.h>
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

// The bootstrap (name) service and ipcc's endpoints: vlaunchd supervises a
// directory with an on-demand service (echo-service, for
// com.vulture.test.echo) and a client (this program with --client), which
// finds the service by name (starting it), calls it through the generated
// proxy, registers a service of its own, and has the echo service started
// again after it stops. The client reports in client.out.

namespace {

char const* const s_directory = "/tmp/bootstrap-test";

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
    (void)write(fd, text, strlen(text));
    close(fd);
}

char const* read_file(char const* name)
{
    static char text[4096];
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

// In the client: a line per finding.
void report(char const* what, bool ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    fflush(stdout);
}

int run_client()
{
    auto port = IPC::look_up("com.vulture.test.echo"_sv);
    report("look_up finds the service", !port.is_error());
    if (port.is_error())
        return 1;
    EchoProxy echo(port.release_value());
    auto text = echo.echo("hello"_sv);
    report("a call starts the service on demand and gets the answer", !text.is_error() && text.value().view() == "hello"_sv);
    auto first = echo.process();
    report("the service is another process", !first.is_error() && first.value() != getpid());
    report("one-way messages", !echo.shout("one"_sv).is_error() && !echo.shout("two"_sv).is_error());
    Vector<String> words;
    words.append(MUST(String::from_view("vulture"_sv)));
    words.append(MUST(String::from_view("os"_sv)));
    auto lengths = echo.lengths(words);
    report("vectors and several results", !lengths.is_error() && lengths.value().lengths.size() == 2 && lengths.value().lengths[0] == 7 && lengths.value().total == 9);

    // The generated proxy's round trip.
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    constexpr int rounds = 2000;
    for (int i = 0; i < rounds; ++i)
        (void)echo.process();
    clock_gettime(CLOCK_MONOTONIC, &end);
    long long nanoseconds = ((end.tv_sec - start.tv_sec) * 1'000'000'000LL + (end.tv_nsec - start.tv_nsec)) / rounds;
    printf("time: a generated call and its answer: %lld ns\n", nanoseconds);

    auto missing = IPC::look_up("com.vulture.test.missing"_sv);
    report("look_up of an unknown name: ENOENT", missing.is_error() && missing.error().code() == ENOENT);
    auto stolen = IPC::check_in("com.vulture.test.echo"_sv);
    report("check_in by someone else: EPERM", stolen.is_error() && stolen.error().code() == EPERM);

    // A service of our own.
    auto mine = IPC::create_port();
    auto send_copy = mine.value().send.duplicate();
    report("register_service", !IPC::register_service("com.vulture.test.mine"_sv, move(mine.value().send)).is_error());
    auto again = IPC::register_service("com.vulture.test.mine"_sv, send_copy.release_value());
    report("the name is taken: EEXIST", again.is_error() && again.error().code() == EEXIST);
    auto found = IPC::look_up("com.vulture.test.mine"_sv);
    IPC::Encoder note(1, 1);
    note.encode("to myself"_sv);
    bool sent = !found.is_error() && !IPC::send(found.value(), note).is_error();
    auto received = IPC::receive(mine.value().receive, true);
    report("look_up finds it; messages arrive", sent && !received.is_error() && received.value().sender.pid == getpid());

    // Stopped, then started again by the next message.
    report("stop", !echo.stop().is_error());
    auto second = echo.process();
    report("the next message starts it again, as a new process", !second.is_error() && second.value() != first.value());
    (void)echo.stop();
    printf("done\n");
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--client") == 0)
        return run_client();

    system("rm -rf /tmp/bootstrap-test");
    mkdir(s_directory, 0755);
    write_file("echo.plist", R"({
        "Label" : "com.vulture.test.echo",
        "ProgramArguments" : [ "/usr/tests/echo-service" ],
        "MachServices" : { "com.vulture.test.echo" : true },
        "ThrottleInterval" : 0,
        "StandardErrorPath" : "/tmp/bootstrap-test/echo.err"
    })");
    write_file("client.plist", R"({
        "Label" : "com.vulture.test.client",
        "ProgramArguments" : [ "/usr/tests/bootstrap", "--client" ],
        "RunAtLoad" : true,
        "StandardOutPath" : "/tmp/bootstrap-test/client.out",
        "StandardErrorPath" : "/tmp/bootstrap-test/client.out"
    })");

    pid_t launchd = fork();
    if (launchd == 0) {
        char const* arguments[] = { "vlaunchd", "--log", "/tmp/bootstrap-test/log", "--directory", s_directory, nullptr };
        execv("/sbin/vlaunchd", const_cast<char**>(arguments));
        _exit(127);
    }
    for (int waited = 0; waited < 200 && !strstr(read_file("client.out"), "done"); ++waited)
        pause_milliseconds(50);
    kill(launchd, SIGTERM);
    int status = 0;
    waitpid(launchd, &status, 0);

    static char report_text[4096];
    snprintf(report_text, sizeof(report_text), "%s", read_file("client.out"));
    char const* const expected[] = {
        "ok: look_up finds the service",
        "ok: a call starts the service on demand and gets the answer",
        "ok: the service is another process",
        "ok: one-way messages",
        "ok: vectors and several results",
        "ok: look_up of an unknown name: ENOENT",
        "ok: check_in by someone else: EPERM",
        "ok: register_service",
        "ok: the name is taken: EEXIST",
        "ok: look_up finds it; messages arrive",
        "ok: stop",
        "ok: the next message starts it again, as a new process",
        "done",
    };
    bool all = true;
    for (auto const* line : expected) {
        if (!strstr(report_text, line)) {
            fprintf(stderr, "bootstrap: missing \"%s\"\n", line);
            all = false;
        }
    }
    CHECK(all);
    if (!all)
        fprintf(stderr, "bootstrap: the client said:\n%s\nvlaunchd's log:\n%s", report_text, read_file("log"));
    if (char const* timing = strstr(report_text, "time: ")) {
        char line[128] = {};
        sscanf(timing, "%127[^\n]", line);
        printf("bootstrap: %s\n", line);
    }
    CHECK(strcmp(read_file("shouts"), "one\ntwo\n") == 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    system("rm -rf /tmp/bootstrap-test");
    return finish();
}
