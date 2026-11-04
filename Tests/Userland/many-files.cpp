/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// As many open files as a real program might want. Which is a lot: it's what a server has.

namespace {

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "many-files: line %d: %s: %s\n", __LINE__, #condition, strerror(errno)); \
            exit(1);                                                                            \
        }                                                                                       \
    } while (0)

constexpr int how_many = 10000;

double now()
{
    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

}

int main()
{
    rlimit limit {};
    CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    limit.rlim_cur = how_many + 64;
    CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);

    static int fds[how_many];
    double started = now();
    for (int& fd : fds) {
        fd = open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
    }
    printf("many-files: %d files open, in %d ms\n", how_many, static_cast<int>((now() - started) * 1000));
    CHECK(fds[how_many - 1] >= how_many - 1);

    // The lowest, and it's not slow to find one, however many there are.
    int const middle = fds[how_many / 2];
    close(middle);
    int const reused = dup(0);
    CHECK(reused == middle);
    fds[how_many / 2] = reused;

    // A pipe, past them all, and poll() for all of them.
    int ends[2];
    CHECK(pipe(ends) == 0);
    CHECK(ends[0] > how_many - 1);
    static pollfd watching[how_many + 1];
    for (int i = 0; i < how_many; ++i)
        watching[i] = { fds[i], 0, 0 };
    watching[how_many] = { ends[0], POLLIN, 0 };
    CHECK(poll(watching, how_many + 1, 0) == 0);
    CHECK(write(ends[1], "x", 1) == 1);
    started = now();
    CHECK(poll(watching, how_many + 1, 1000) == 1 && (watching[how_many].revents & POLLIN));
    printf("many-files: poll() of all of them, in %d us\n", static_cast<int>((now() - started) * 1000000));

    // kqueue only has to look at what it's been told about.
    int const queue = kqueue();
    CHECK(queue >= 0);
    struct kevent change;
    EV_SET(&change, ends[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
    CHECK(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0);
    struct kevent event;
    timespec none { 0, 0 };
    CHECK(kevent(queue, nullptr, 0, &event, 1, &none) == 1 && event.ident == static_cast<uintptr_t>(ends[0]));
    close(queue);

    // A child has them all too, and they're the same files.
    started = now();
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        char byte;
        _exit(read(ends[0], &byte, 1) == 1 && byte == 'x' ? 0 : 1);
    }
    int status = -1;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("many-files: fork() with all of them, and back, in %d ms\n", static_cast<int>((now() - started) * 1000));

    // Going past the limit isn't anything worse than being told.
    errno = 0;
    limit.rlim_cur = how_many + 2;
    CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    bool told = false;
    for (int i = 0; i < 16 && !told; ++i)
        told = dup(0) < 0 && errno == EMFILE;
    CHECK(told);
    printf("many-files: past the limit: EMFILE\n");
    limit.rlim_cur = how_many + 64;
    CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);

    started = now();
    for (int fd : fds)
        CHECK(close(fd) == 0);
    printf("many-files: closed again, in %d ms\n", static_cast<int>((now() - started) * 1000));
    int const low = dup(0);
    CHECK(low < 64);
    printf("many-files: PASS\n");
    return 0;
}
