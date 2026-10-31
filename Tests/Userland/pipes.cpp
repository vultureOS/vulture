/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int volatile s_sigpipes = 0;

int main()
{
    // A message from child to parent.
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        char const message[] = "through the pipe";
        write(fds[1], message, sizeof(message));
        _exit(0);
    }
    close(fds[1]);
    char buffer[64] {};
    CHECK(read(fds[0], buffer, sizeof(buffer)) == 17 && strcmp(buffer, "through the pipe") == 0);
    // Every writer is gone: end of file.
    CHECK(read(fds[0], buffer, sizeof(buffer)) == 0);
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);

    // More than the buffer holds: the writer blocks until the reader drains.
    CHECK(pipe(fds) == 0);
    constexpr size_t total = 300'000;
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        static unsigned char chunk[10'000];
        for (size_t sent = 0; sent < total; sent += sizeof(chunk)) {
            for (size_t i = 0; i < sizeof(chunk); ++i)
                chunk[i] = static_cast<unsigned char>((sent + i) % 251);
            if (write(fds[1], chunk, sizeof(chunk)) != static_cast<ssize_t>(sizeof(chunk)))
                _exit(1);
        }
        _exit(0);
    }
    close(fds[1]);
    size_t received = 0;
    bool in_order = true;
    unsigned char block[4096];
    for (;;) {
        ssize_t length = read(fds[0], block, sizeof(block));
        if (length <= 0)
            break;
        for (ssize_t i = 0; i < length; ++i)
            in_order = in_order && block[i] == static_cast<unsigned char>((received + static_cast<size_t>(i)) % 251);
        received += static_cast<size_t>(length);
    }
    CHECK(received == total && in_order);
    close(fds[0]);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // No reader left: EPIPE and SIGPIPE.
    signal(SIGPIPE, [](int) { s_sigpipes = s_sigpipes + 1; });
    CHECK(pipe(fds) == 0);
    close(fds[0]);
    CHECK(write(fds[1], "x", 1) == -1 && errno == EPIPE);
    CHECK(s_sigpipes == 1);
    close(fds[1]);

    // Non-blocking reads, and dup2 redirecting a standard stream.
    CHECK(pipe2(fds, O_NONBLOCK) == 0);
    CHECK(read(fds[0], buffer, 1) == -1 && errno == EAGAIN);
    int saved = dup(STDOUT_FILENO);
    CHECK(dup2(fds[1], STDOUT_FILENO) == STDOUT_FILENO);
    write(STDOUT_FILENO, "redirected", 10);
    CHECK(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved);
    memset(buffer, 0, sizeof(buffer));
    CHECK(read(fds[0], buffer, sizeof(buffer)) == 10 && strcmp(buffer, "redirected") == 0);
    CHECK(fcntl(fds[0], F_GETFD) == 0 && fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(fds[0], F_GETFD) == FD_CLOEXEC);
    close(fds[0]);
    close(fds[1]);
    return finish();
}
