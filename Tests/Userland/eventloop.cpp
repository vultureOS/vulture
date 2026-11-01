/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibCore/EventLoop.h>
#include <LibJSON/JSON.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Core::EventLoop: deferred calls, timers in deadline order, repeating
// timers, file descriptors, signals (SIGCHLD from a child included), and
// callbacks that add or remove things, themselves included. Then LibJSON,
// running on vultureOS.

namespace {

u64 now_milliseconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1000 + static_cast<u64>(now.tv_nsec) / 1'000'000;
}

void test_deferred_and_timers(Core::EventLoop& loop)
{
    char order[16] = {};
    size_t length = 0;
    auto note = [&](char c) { order[length++] = c; };

    loop.deferred_invoke([&] { note('a'); });
    loop.deferred_invoke([&] {
        note('b');
        // Made while running: next round.
        loop.deferred_invoke([&] { note('c'); });
    });
    u64 started = now_milliseconds();
    loop.add_timer(60, false, [&] { note('3'); });
    loop.add_timer(20, false, [&] { note('1'); });
    loop.add_timer(40, false, [&] { note('2'); });
    int cancelled = loop.add_timer(50, false, [&] { note('x'); });
    loop.add_timer(30, false, [&] { loop.cancel_timer(cancelled); });
    int ticks = 0;
    int ticker = 0;
    ticker = loop.add_timer(5, true, [&] {
        if (++ticks == 3)
            loop.cancel_timer(ticker);
    });
    loop.add_timer(80, false, [&] { loop.quit(7); });
    CHECK(loop.exec() == 7);
    u64 took = now_milliseconds() - started;
    if (strcmp(order, "abc123") != 0)
        fprintf(stderr, "eventloop: order \"%s\"\n", order);
    CHECK(strcmp(order, "abc123") == 0);
    CHECK(ticks == 3);
    CHECK(took >= 80);
}

void test_file_descriptors(Core::EventLoop& loop)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    char received[64] = {};
    size_t length = 0;
    int reader = 0;
    reader = loop.watch(fds[0], Core::EventLoop::Interest::Read, [&] {
        ssize_t got = read(fds[0], received + length, sizeof(received) - 1 - length);
        if (got <= 0) {
            // End of file: the writer closed.
            loop.unwatch(reader);
            loop.quit(0);
            return;
        }
        length += static_cast<size_t>(got);
    });
    int writes = 0;
    int writer = 0;
    writer = loop.add_timer(5, true, [&] {
        char const* pieces[] = { "one ", "two ", "three" };
        (void)write(fds[1], pieces[writes], strlen(pieces[writes]));
        if (++writes == 3) {
            loop.cancel_timer(writer);
            close(fds[1]);
        }
    });
    // Writable at once.
    int writable_calls = 0;
    int writable = 0;
    int other[2];
    CHECK(pipe(other) == 0);
    writable = loop.watch(other[1], Core::EventLoop::Interest::Write, [&] {
        ++writable_calls;
        loop.unwatch(writable);
    });
    CHECK(loop.exec() == 0);
    CHECK(strcmp(received, "one two three") == 0);
    CHECK(writable_calls == 1);
    close(fds[0]);
    close(other[0]);
    close(other[1]);
}

void test_signals(Core::EventLoop& loop)
{
    int first_calls = 0, second_calls = 0;
    int first = 0;
    first = loop.on_signal(SIGUSR1, [&] {
        ++first_calls;
        // Removing itself does not stop the next handler.
        loop.remove_signal_handler(first);
    });
    loop.on_signal(SIGUSR1, [&] { ++second_calls; });
    raise(SIGUSR1);
    loop.pump(false);
    CHECK(first_calls == 1);
    CHECK(second_calls == 1);
    raise(SIGUSR1);
    loop.pump(false);
    CHECK(first_calls == 1);
    CHECK(second_calls == 2);

    // SIGCHLD: a child ends; the loop hears of it and reaps it.
    int status = -1;
    pid_t reaped = 0;
    loop.on_signal(SIGCHLD, [&] {
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            reaped = pid;
            loop.quit(0);
        }
    });
    pid_t child = fork();
    if (child == 0)
        _exit(5);
    loop.add_timer(5000, false, [&] { loop.quit(1); });
    CHECK(loop.exec() == 0);
    CHECK(reaped == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5);
}

void test_json()
{
    auto parsed = JSON::parse(R"({"Label":"com.example","Interval":2.5,"Arguments":["a","b"]})"_sv);
    CHECK(!parsed.is_error());
    if (parsed.is_error())
        return;
    auto const& value = parsed.value();
    CHECK(value.get_string("Label"_sv).value() == "com.example"_sv);
    CHECK(value.get_number("Interval"_sv).value() == 2.5);
    CHECK(value.get_array("Arguments"_sv)->size() == 2);
    auto text = value.serialize();
    CHECK(!text.is_error() && text.value().view() == R"({"Label":"com.example","Interval":2.5,"Arguments":["a","b"]})"_sv);
}

}

int main()
{
    Core::EventLoop loop;
    test_deferred_and_timers(loop);
    test_file_descriptors(loop);
    test_signals(loop);
    test_json();
    return finish();
}
