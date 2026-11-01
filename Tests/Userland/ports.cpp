/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibCore/EventLoop.h>
#include <LibIPC/Listener.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/port.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Ports: messages with bytes, descriptors, reply rights and out-of-line
// memory; the queue's limits; calls and their answers; what happens when
// either end goes away; poll() and the event loop; and the round trip's
// speed.

namespace {

vulture_port_message message_of(char const* text, unsigned id = 1)
{
    vulture_port_message message {};
    message.id = id;
    message.bytes = text;
    message.size = strlen(text);
    message.reply = -1;
    return message;
}

struct Inbox {
    char bytes[256] {};
    int rights[VULTURE_PORT_MAX_RIGHTS] {};
    vulture_port_region regions[VULTURE_PORT_MAX_REGIONS] {};
    vulture_port_received received {};

    Inbox()
    {
        received.bytes = bytes;
        received.capacity = sizeof(bytes) - 1;
        received.rights = rights;
        received.right_capacity = VULTURE_PORT_MAX_RIGHTS;
        received.regions = regions;
        received.region_capacity = VULTURE_PORT_MAX_REGIONS;
    }

    int take(int port, int flags = 0)
    {
        int result = port_receive(port, &received, flags);
        if (result == 0)
            bytes[received.size] = '\0';
        return result;
    }
};

// A server in a child process: answers each call with the request's bytes
// in capitals, until a request says "quit".
pid_t start_upper_server(int receive_right)
{
    pid_t child = fork();
    if (child == 0) {
        for (;;) {
            Inbox inbox;
            if (inbox.take(receive_right) < 0)
                _exit(1);
            bool quit = strcmp(inbox.bytes, "quit") == 0;
            for (char* c = inbox.bytes; *c; ++c)
                *c = static_cast<char>(toupper(*c));
            // The caller's identity rides along: put it in the answer.
            char answer[300];
            snprintf(answer, sizeof(answer), "%s from %d", inbox.bytes, inbox.received.sender_pid);
            auto reply = message_of(answer, inbox.received.id + 1);
            port_send(inbox.received.reply, &reply, 0);
            close(inbox.received.reply);
            if (quit)
                _exit(0);
        }
    }
    return child;
}

void test_bytes_and_limits()
{
    int fds[2];
    CHECK(port_create(fds, O_CLOEXEC) == 0);
    struct stat information;
    CHECK(fstat(fds[0], &information) == 0 && S_ISSOCK(information.st_mode));
    CHECK(fcntl(fds[0], F_GETFD) == FD_CLOEXEC);

    auto hello = message_of("hello", 7);
    CHECK(port_send(fds[1], &hello, 0) == 0);
    Inbox inbox;
    CHECK(inbox.take(fds[0]) == 0);
    CHECK(strcmp(inbox.bytes, "hello") == 0 && inbox.received.id == 7);
    CHECK(inbox.received.sender_pid == getpid() && inbox.received.sender_uid == geteuid());
    CHECK(inbox.received.right_count == 0 && inbox.received.reply == -1 && inbox.received.region_count == 0);

    // Empty: EAGAIN without waiting.
    errno = 0;
    CHECK(inbox.take(fds[0], VULTURE_PORT_NONBLOCK) == -1 && errno == EAGAIN);

    // Full after 64.
    int sent = 0;
    auto small = message_of("x");
    while (port_send(fds[1], &small, VULTURE_PORT_NONBLOCK) == 0)
        ++sent;
    CHECK(sent == VULTURE_PORT_QUEUE_LIMIT && errno == EAGAIN);
    struct pollfd poller { fds[1], POLLOUT, 0 };
    CHECK(poll(&poller, 1, 0) == 0);
    for (int i = 0; i < sent; ++i)
        CHECK(inbox.take(fds[0]) == 0);

    // Too big for the buffer: EMSGSIZE, the size told, the message kept.
    char big[1000];
    memset(big, 'b', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    auto large = message_of(big);
    CHECK(port_send(fds[1], &large, 0) == 0);
    errno = 0;
    CHECK(inbox.take(fds[0]) == -1 && errno == EMSGSIZE && inbox.received.size == 999);
    static char room[1024];
    inbox.received.bytes = room;
    inbox.received.capacity = sizeof(room);
    CHECK(port_receive(fds[0], &inbox.received, 0) == 0 && inbox.received.size == 999 && room[998] == 'b');

    // Not byte streams.
    errno = 0;
    CHECK(write(fds[1], "x", 1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(port_send(fds[0], &hello, 0) == -1 && errno == EINVAL);
    int pipe_fds[2];
    pipe(pipe_fds);
    errno = 0;
    CHECK(port_send(pipe_fds[1], &hello, 0) == -1 && errno == ENOTSOCK);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    vulture_port_message huge {};
    huge.size = VULTURE_PORT_MAX_INLINE + 1;
    huge.bytes = big;
    huge.reply = -1;
    errno = 0;
    CHECK(port_send(fds[1], &huge, 0) == -1 && errno == EMSGSIZE);
    close(fds[0]);
    close(fds[1]);
}

void test_rights()
{
    int port[2], other[2], pipe_fds[2];
    CHECK(port_create(port, 0) == 0 && port_create(other, 0) == 0 && pipe(pipe_fds) == 0);
    // A pipe's write end and another port's send right, through the port.
    vulture_port_message message {};
    int rights[2] = { pipe_fds[1], other[1] };
    message.id = 1;
    message.rights = rights;
    message.right_count = 2;
    message.reply = -1;
    CHECK(port_send(port[1], &message, 0) == 0);
    Inbox inbox;
    CHECK(inbox.take(port[0]) == 0 && inbox.received.right_count == 2);
    int pipe_copy = inbox.rights[0], port_copy = inbox.rights[1];
    CHECK(pipe_copy != pipe_fds[1] && fcntl(pipe_copy, F_GETFD) == FD_CLOEXEC);
    CHECK(write(pipe_copy, "via port", 8) == 8);
    char text[16] = {};
    CHECK(read(pipe_fds[0], text, sizeof(text)) == 8 && strcmp(text, "via port") == 0);
    auto through = message_of("through");
    CHECK(port_send(port_copy, &through, 0) == 0);
    CHECK(inbox.take(other[0]) == 0 && strcmp(inbox.bytes, "through") == 0);

    // VULTURE_PORT_MOVE: the sender's copy is gone.
    message.rights = &pipe_fds[1];
    message.right_count = 1;
    message.flags = VULTURE_PORT_MOVE;
    CHECK(port_send(port[1], &message, 0) == 0);
    CHECK(fcntl(pipe_fds[1], F_GETFD) == -1 && errno == EBADF);
    CHECK(inbox.take(port[0]) == 0 && inbox.received.right_count == 1);
    close(inbox.rights[0]);

    // Another send right, made from the receive right.
    int made = port_make_send(port[0], 0);
    CHECK(made >= 0);
    auto again = message_of("again");
    CHECK(port_send(made, &again, 0) == 0 && inbox.take(port[0]) == 0 && strcmp(inbox.bytes, "again") == 0);
    errno = 0;
    CHECK(port_make_send(port[1], 0) == -1 && errno == EINVAL);
    int const opened[] = { port[0], port[1], other[0], other[1], pipe_fds[0], pipe_copy, port_copy, made };
    for (int fd : opened)
        close(fd);
}

void test_calls_and_ends()
{
    int fds[2];
    CHECK(port_create(fds, 0) == 0);
    pid_t server = start_upper_server(fds[0]);
    close(fds[0]);
    auto request = message_of("hello", 10);
    Inbox answer;
    CHECK(port_call(fds[1], &request, &answer.received, 0) == 0);
    answer.bytes[answer.received.size] = '\0';
    char expected[64];
    snprintf(expected, sizeof(expected), "HELLO from %d", getpid());
    CHECK(strcmp(answer.bytes, expected) == 0 && answer.received.id == 11);
    CHECK(answer.received.sender_pid == server);

    auto quit = message_of("quit");
    Inbox last;
    CHECK(port_call(fds[1], &quit, &last.received, 0) == 0);
    int status = 0;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    // The receive right died with the server.
    errno = 0;
    CHECK(port_send(fds[1], &request, 0) == -1 && errno == EPIPE);
    struct pollfd poller { fds[1], POLLOUT, 0 };
    CHECK(poll(&poller, 1, 0) == 1 && (poller.revents & POLLHUP));
    close(fds[1]);

    // A server that takes the call and goes away without answering.
    CHECK(port_create(fds, 0) == 0);
    pid_t silent = fork();
    if (silent == 0) {
        Inbox inbox;
        inbox.take(fds[0]);
        _exit(0);
    }
    close(fds[0]);
    Inbox never;
    errno = 0;
    CHECK(port_call(fds[1], &request, &never.received, 0) == -1 && errno == EPIPE);
    waitpid(silent, nullptr, 0);
    close(fds[1]);

    // No send rights left and nothing queued: receiving ends, EPIPE.
    CHECK(port_create(fds, 0) == 0);
    CHECK(port_send(fds[1], &request, 0) == 0);
    close(fds[1]);
    Inbox inbox;
    CHECK(inbox.take(fds[0]) == 0);
    errno = 0;
    CHECK(inbox.take(fds[0]) == -1 && errno == EPIPE);
    poller = { fds[0], POLLIN, 0 };
    CHECK(poll(&poller, 1, 0) == 1 && (poller.revents & POLLHUP));
    close(fds[0]);
}

void test_memory()
{
    int fds[2];
    CHECK(port_create(fds, 0) == 0);
    constexpr size_t size = 3 * 4096;
    auto* pages = static_cast<char*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    for (size_t i = 0; i < size; ++i)
        pages[i] = static_cast<char>('a' + i % 26);
    vulture_port_message message {};
    vulture_port_region region { pages, size };
    message.id = 1;
    message.regions = &region;
    message.region_count = 1;
    message.reply = -1;
    CHECK(port_send(fds[1], &message, 0) == 0);
    // Written after sending: the receiver must not see it.
    pages[0] = '!';
    Inbox inbox;
    CHECK(inbox.take(fds[0]) == 0 && inbox.received.region_count == 1 && inbox.regions[0].size == size);
    auto* received = static_cast<char*>(inbox.regions[0].address);
    CHECK(received != pages && received[0] == 'a' && received[size - 1] == static_cast<char>('a' + (size - 1) % 26));
    // Written by the receiver: the sender must not see it.
    received[1] = '?';
    CHECK(pages[1] == 'b');
    CHECK(munmap(received, size) == 0);

    // Moved: gone from the sender.
    message.flags = VULTURE_PORT_MOVE;
    CHECK(port_send(fds[1], &message, 0) == 0);
    int status = 0;
    pid_t child = fork();
    if (child == 0) {
        volatile char c = pages[0];
        (void)c;
        _exit(0);
    }
    waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(inbox.take(fds[0]) == 0 && static_cast<char*>(inbox.regions[0].address)[0] == '!');

    // Not page-aligned, or a file's: refused.
    region = { pages + 1, 4096 };
    message.flags = 0;
    errno = 0;
    CHECK(port_send(fds[1], &message, 0) == -1 && errno == EINVAL);
    close(fds[0]);
    close(fds[1]);
}

void test_event_loop()
{
    int fds[2];
    CHECK(port_create(fds, 0) == 0);
    Core::EventLoop loop;
    int received = 0;
    loop.watch(fds[0], Core::EventLoop::Interest::Read, [&] {
        Inbox inbox;
        while (inbox.take(fds[0], VULTURE_PORT_NONBLOCK) == 0)
            ++received;
        if (received == 3)
            loop.quit(0);
    });
    loop.add_timer(10, false, [&] {
        auto message = message_of("tick");
        for (int i = 0; i < 3; ++i)
            port_send(fds[1], &message, 0);
    });
    loop.add_timer(5000, false, [&] { loop.quit(1); });
    CHECK(loop.exec() == 0 && received == 3);
    close(fds[0]);
    close(fds[1]);
}

// The round trip: a call to another process and its answer.
void measure_round_trip()
{
    int fds[2];
    CHECK(port_create(fds, 0) == 0);
    pid_t server = fork();
    if (server == 0) {
        close(fds[1]);
        char byte[8];
        vulture_port_received request {};
        for (;;) {
            request.bytes = byte;
            request.capacity = sizeof(byte);
            if (port_receive(fds[0], &request, 0) < 0)
                _exit(0);
            vulture_port_message answer {};
            answer.reply = -1;
            port_send(request.reply, &answer, 0);
            close(request.reply);
        }
    }
    close(fds[0]);
    vulture_port_message request {};
    request.reply = -1;
    vulture_port_received answer {};
    constexpr int warmup = 200, rounds = 5000;
    for (int i = 0; i < warmup; ++i)
        port_call(fds[1], &request, &answer, 0);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int failures = 0;
    for (int i = 0; i < rounds; ++i)
        failures += port_call(fds[1], &request, &answer, 0) < 0;
    clock_gettime(CLOCK_MONOTONIC, &end);
    long long nanoseconds = ((end.tv_sec - start.tv_sec) * 1'000'000'000LL + (end.tv_nsec - start.tv_nsec)) / rounds;
    printf("ports: round trip (call and answer, two processes): %lld ns\n", nanoseconds);
    CHECK(failures == 0);
    close(fds[1]);
    waitpid(server, nullptr, 0);
}

}

// The same, with the server waiting in its event loop (poll()) rather
// than in port_receive(), as services do.
void measure_round_trip_through_event_loop()
{
    int fds[2];
    CHECK(port_create(fds, 0) == 0);
    pid_t server = fork();
    if (server == 0) {
        close(fds[1]);
        Core::EventLoop loop;
        loop.watch(fds[0], Core::EventLoop::Interest::Read, [&] {
            char byte[8];
            vulture_port_received request {};
            for (;;) {
                request.bytes = byte;
                request.capacity = sizeof(byte);
                if (port_receive(fds[0], &request, VULTURE_PORT_NONBLOCK) < 0) {
                    if (errno == EPIPE)
                        loop.quit(0);
                    return;
                }
                vulture_port_message answer {};
                answer.reply = -1;
                port_send(request.reply, &answer, 0);
                close(request.reply);
            }
        });
        _exit(loop.exec());
    }
    close(fds[0]);
    vulture_port_message request {};
    request.reply = -1;
    vulture_port_received answer {};
    for (int i = 0; i < 200; ++i)
        port_call(fds[1], &request, &answer, 0);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    constexpr int rounds = 2000;
    int failures = 0;
    for (int i = 0; i < rounds; ++i)
        failures += port_call(fds[1], &request, &answer, 0) < 0;
    clock_gettime(CLOCK_MONOTONIC, &end);
    long long nanoseconds = ((end.tv_sec - start.tv_sec) * 1'000'000'000LL + (end.tv_nsec - start.tv_nsec)) / rounds;
    printf("ports: round trip to a server in its event loop: %lld ns\n", nanoseconds);
    CHECK(failures == 0);
    close(fds[1]);
    waitpid(server, nullptr, 0);
}

// The same through LibIPC: its listener receives, the answer goes back
// with IPC::answer().
void measure_listener()
{
    int fds[2];
    CHECK(port_create(fds, 0) == 0);
    pid_t server = fork();
    if (server == 0) {
        close(fds[1]);
        Core::EventLoop loop;
        IPC::Listener listener(loop, IPC::Port(fds[0]), [&](IPC::ReceivedMessage& message) {
            IPC::Encoder answer(1, 1);
            (void)IPC::answer(message, answer);
        });
        _exit(loop.exec());
    }
    close(fds[0]);
    vulture_port_message request {};
    request.reply = -1;
    vulture_port_received answer {};
    char bytes[64];
    for (int i = 0; i < 200; ++i) {
        answer.bytes = bytes;
        answer.capacity = sizeof(bytes);
        port_call(fds[1], &request, &answer, 0);
    }
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    constexpr int rounds = 2000;
    for (int i = 0; i < rounds; ++i) {
        answer.bytes = bytes;
        answer.capacity = sizeof(bytes);
        port_call(fds[1], &request, &answer, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("ports: round trip to a LibIPC listener: %lld ns\n", ((end.tv_sec - start.tv_sec) * 1'000'000'000LL + (end.tv_nsec - start.tv_nsec)) / rounds);
    kill(server, SIGKILL);
    waitpid(server, nullptr, 0);
    close(fds[1]);
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    test_bytes_and_limits();
    test_rights();
    test_calls_and_ends();
    test_memory();
    test_event_loop();
    measure_round_trip();
    measure_round_trip_through_event_loop();
    measure_listener();
    return finish();
}
