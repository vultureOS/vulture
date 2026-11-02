/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NativeMachine.h"
#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

void test_system_information()
{
    struct utsname name {};
    CHECK(uname(&name) == 0);
    CHECK(strcmp(name.sysname, "vultureOS") == 0 && strcmp(name.machine, NATIVE_MACHINE) == 0);
    int processors = 0;
    size_t length = sizeof(processors);
    CHECK(sysctlbyname("hw.ncpu", &processors, &length, nullptr, 0) == 0 && processors >= 1 && processors == sysconf(_SC_NPROCESSORS_ONLN));
    uint64_t memory = 0;
    length = sizeof(memory);
    CHECK(sysctlbyname("hw.memsize", &memory, &length, nullptr, 0) == 0 && memory > 128ull * 1024 * 1024);
    char text[64];
    length = sizeof(text);
    CHECK(sysctlbyname("kern.ostype", text, &length, nullptr, 0) == 0 && strcmp(text, "vultureOS") == 0);
    CHECK(sysctlbyname("no.such.thing", text, &length, nullptr, 0) < 0 && errno == ENOENT);

    char original[64];
    CHECK(gethostname(original, sizeof(original)) == 0);
    CHECK(sethostname("test-host", 9) == 0);
    CHECK(uname(&name) == 0 && strcmp(name.nodename, "test-host") == 0);
    CHECK(sethostname(original, strlen(original)) == 0);

    unsigned char first[32] {}, second[32] {};
    CHECK(getrandom(first, sizeof(first), 0) == 32 && getrandom(second, sizeof(second), 0) == 32);
    CHECK(memcmp(first, second, sizeof(first)) != 0);
    CHECK(arc4random_uniform(10) < 10);
}

void test_poll()
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    struct pollfd watch[2] = { { fds[0], POLLIN, 0 }, { fds[1], POLLOUT, 0 } };
    CHECK(poll(watch, 1, 0) == 0 && watch[0].revents == 0);
    CHECK(poll(watch, 2, 0) == 1 && (watch[1].revents & POLLOUT));
    // Waits out the timeout when nothing comes.
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    CHECK(poll(watch, 1, 100) == 0);
    clock_gettime(CLOCK_MONOTONIC, &after);
    long long waited = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    CHECK(waited >= 95 && waited < 2000);
    // Wakes when data arrives from another process.
    pid_t writer = fork();
    if (writer == 0) {
        struct timespec pause { 0, 50 * 1000 * 1000 };
        nanosleep(&pause, nullptr);
        (void)write(fds[1], "x", 1);
        _exit(0);
    }
    CHECK(poll(watch, 1, 5000) == 1 && (watch[0].revents & POLLIN));
    waitpid(writer, nullptr, 0);
    close(fds[1]);
    char byte;
    CHECK(read(fds[0], &byte, 1) == 1);
    CHECK(poll(watch, 1, 0) == 1 && (watch[0].revents & (POLLIN | POLLHUP)));
    close(fds[0]);
}

void test_permissions()
{
    CHECK(getuid() == 0 && geteuid() == 0);
    struct stat information {};
    CHECK(stat("/tmp", &information) == 0 && (information.st_mode & 07777) == 01777);

    CHECK(mkdir("/tmp/private", 0755) == 0);
    int fd = open("/tmp/private/secret", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1);
    close(fd);
    fd = open("/tmp/rootfile", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    close(fd);

    // umask takes bits away from what is created.
    mode_t old = umask(077);
    fd = open("/tmp/masked", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    close(fd);
    CHECK(stat("/tmp/masked", &information) == 0 && (information.st_mode & 0777) == 0600);
    CHECK(umask(old) == 077);

    // Hard links share an inode.
    CHECK(link("/tmp/private/secret", "/tmp/private/also") == 0);
    struct stat a {}, b {};
    CHECK(stat("/tmp/private/secret", &a) == 0 && stat("/tmp/private/also", &b) == 0);
    CHECK(a.st_ino == b.st_ino && a.st_nlink == 2);
    CHECK(link("/tmp/private", "/tmp/dirlink") < 0 && errno == EPERM);

    // Times.
    struct timespec times[2] = { { 1000, 5 }, { 2000, 7 } };
    CHECK(utimensat(AT_FDCWD, "/tmp/rootfile", times, 0) == 0);
    CHECK(stat("/tmp/rootfile", &information) == 0 && information.st_mtim.tv_sec == 2000 && information.st_atim.tv_sec == 1000);

    pid_t child = fork();
    if (child == 0) {
        // An ordinary user, in group 20.
        gid_t groups[] = { 20 };
        if (setgroups(1, groups) < 0 || setgid(20) < 0 || setuid(501) < 0)
            _exit(50);
        if (getuid() != 501 || geteuid() != 501 || getgid() != 20)
            _exit(51);
        if (setuid(0) == 0 || errno != EPERM)
            _exit(52);
        if (open("/tmp/private/secret", O_RDONLY) >= 0 || errno != EACCES)
            _exit(53);
        if (open("/tmp/private/new", O_WRONLY | O_CREAT, 0644) >= 0 || errno != EACCES)
            _exit(54);
        if (chmod("/tmp/private/secret", 0777) == 0 || errno != EPERM)
            _exit(55);
        // /tmp lets anyone create; the file is ours.
        int mine = open("/tmp/users-file", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        struct stat own {};
        if (mine < 0 || fstat(mine, &own) < 0 || own.st_uid != 501 || own.st_gid != 20)
            _exit(56);
        close(mine);
        if (chown("/tmp/users-file", 0, 20) == 0 || errno != EPERM)
            _exit(57);
        if (chmod("/tmp/users-file", 0600) < 0)
            _exit(58);
        // The sticky bit: not someone else's file.
        if (unlink("/tmp/rootfile") == 0 || errno != EPERM)
            _exit(59);
        if (kill(1, SIGTERM) == 0 || errno != EPERM)
            _exit(60);
        if (sethostname("evil", 4) == 0 || errno != EPERM)
            _exit(61);
        if (unlink("/tmp/users-file") < 0)
            _exit(62);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    if (WEXITSTATUS(status) != 0)
        fprintf(stderr, "permissions: child failed step %d\n", WEXITSTATUS(status));
    CHECK(WEXITSTATUS(status) == 0);

    // Root may change owners.
    CHECK(chown("/tmp/rootfile", 501, 20) == 0);
    CHECK(stat("/tmp/rootfile", &information) == 0 && information.st_uid == 501 && information.st_gid == 20);
    CHECK(unlink("/tmp/rootfile") == 0 && unlink("/tmp/masked") == 0);
    CHECK(unlink("/tmp/private/also") == 0 && unlink("/tmp/private/secret") == 0 && rmdir("/tmp/private") == 0);
}

}

int main()
{
    test_system_information();
    test_poll();
    test_permissions();
    return finish();
}
