/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NativeMachine.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Our POSIX conformance subset (Documentation/POSIX.md): what POSIX.1-2017
// requires of the interfaces vultureOS has, area by area. Each failure
// names the requirement; each area reports how many it meets.

extern char** environ;

namespace {

char const* s_area = "";
int s_area_passed = 0, s_area_failed = 0;
int s_total_passed = 0, s_total_failed = 0;

void begin(char const* area)
{
    s_area = area;
    s_area_passed = s_area_failed = 0;
}

void end_area()
{
    printf("posix: %-12s %3d of %3d\n", s_area, s_area_passed, s_area_passed + s_area_failed);
    fflush(stdout);
}

void require(bool ok, char const* what, int line)
{
    if (ok) {
        ++s_area_passed;
        ++s_total_passed;
        // Each by name, for comparing with what a real Mac says (Meta/oracle-test.sh).
        printf("posix: %s: ok: %s\n", s_area, what);
        return;
    }
    ++s_area_failed;
    ++s_total_failed;
    fprintf(stderr, "posix: %s: FAIL (posix.cpp:%d): %s (errno %d: %s)\n", s_area, line, what, errno, strerror(errno));
}

#define REQUIRE(condition, what) require((condition), what, __LINE__)

char const* const s_dir = "/tmp/posix";

char const* path(char const* name)
{
    static char buffers[4][256];
    static int next = 0;
    char* buffer = buffers[next++ % 4];
    snprintf(buffer, 256, "%s/%s", s_dir, name);
    return buffer;
}

void write_file(char const* name, char const* text)
{
    int fd = open(path(name), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, text, strlen(text));
        close(fd);
    }
}

// Runs `/bin/sh -c script` (posix_spawn); its exit status, and its output
// in `output` if given.
int shell(char const* script, char* output = nullptr, size_t size = 0, char* const* environment = environ)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    char const* argv[] = { "sh", "-c", script, nullptr };
    pid_t child = -1;
    int error = posix_spawn(&child, "/bin/sh", &actions, nullptr, const_cast<char**>(argv), environment);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    size_t length = 0;
    char ignored[256];
    ssize_t got;
    while ((got = read(fds[0], output ? output + length : ignored, output ? size - 1 - length : sizeof(ignored))) > 0) {
        if (output)
            length += static_cast<size_t>(got);
    }
    if (output)
        output[length] = '\0';
    close(fds[0]);
    if (error)
        return -1;
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Runs `body` in a child; how it ended.
template<typename Body>
int in_child(Body body)
{
    pid_t child = fork();
    if (child == 0) {
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return status;
}

long long milliseconds_since(struct timespec const& start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start.tv_sec) * 1000LL + (now.tv_nsec - start.tv_nsec) / 1'000'000;
}

volatile sig_atomic_t s_signal_count = 0;
volatile sig_atomic_t s_last_signal = 0;
volatile int s_info_signal = 0;
volatile pid_t s_info_pid = 0;

void count_signal(int signal)
{
    s_signal_count = s_signal_count + 1;
    s_last_signal = signal;
}

void record_info(int signal, siginfo_t* info, void*)
{
    s_info_signal = info->si_signo == signal ? signal : -1;
    s_info_pid = info->si_pid;
}

void processes()
{
    begin("processes");
    pid_t parent = getpid();
    int fds[2];
    REQUIRE(pipe(fds) == 0, "pipe() makes a pipe");
    pid_t child = fork();
    if (child == 0) {
        pid_t ids[2] = { getpid(), getppid() };
        (void)write(fds[1], ids, sizeof(ids));
        _exit(42);
    }
    pid_t ids[2] = {};
    (void)read(fds[0], ids, sizeof(ids));
    close(fds[0]);
    close(fds[1]);
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child, "waitpid() returns the child it reaped");
    REQUIRE(ids[0] == child && ids[0] != parent, "fork(): the child has a process ID of its own, the one fork() returned");
    REQUIRE(ids[1] == parent, "fork(): the child's parent is the forking process");
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 42, "_exit(42): WIFEXITED, WEXITSTATUS 42");

    child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    REQUIRE(waitpid(child, &status, WNOHANG) == 0, "waitpid(WNOHANG) returns 0 while the child runs");
    kill(child, SIGSTOP);
    REQUIRE(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP, "WUNTRACED reports a stopped child");
    kill(child, SIGCONT);
    REQUIRE(waitpid(child, &status, WCONTINUED) == child && WIFCONTINUED(status), "WCONTINUED reports a continued child");
    kill(child, SIGTERM);
    REQUIRE(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "a child killed by SIGTERM: WIFSIGNALED, WTERMSIG");
    errno = 0;
    REQUIRE(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "waitpid() with no children: ECHILD");
    errno = 0;
    REQUIRE(kill(child, 0) == -1 && errno == ESRCH, "kill(pid, 0) for a process that is gone: ESRCH");
    REQUIRE(kill(getpid(), 0) == 0, "kill(pid, 0) for a process that exists: 0");

    char* const argv[] = { const_cast<char*>("x"), nullptr };
    status = in_child([&] { _exit(execv("/nonexistent/program", argv) == -1 && errno == ENOENT ? 0 : 1); });
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execv() of a missing file: ENOENT");
    status = in_child([&] { _exit(execv("/tmp", argv) == -1 && errno == EACCES ? 0 : 1); });
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execv() of a directory: EACCES");
    write_file("data", "not a program\n");
    chmod(path("data"), 0644);
    status = in_child([&] { _exit(execv(path("data"), argv) == -1 && errno == EACCES ? 0 : 1); });
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execv() of a file without execute permission: EACCES");

    status = in_child([] {
        if (setpgid(0, 0) < 0 || getpgrp() != getpid() || getpgid(0) != getpid())
            _exit(1);
        // A process group leader may not start a session.
        if (setsid() != -1 || errno != EPERM)
            _exit(2);
        _exit(0);
    });
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "setpgid(0, 0) makes a group led by the caller; its leader may not setsid(): EPERM");
    status = in_child([] {
        pid_t session = setsid();
        _exit(session == getpid() && getsid(0) == getpid() && getpgrp() == getpid() ? 0 : 1);
    });
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "setsid() makes the caller leader of a new session and group");

    // posix_spawn.
    char output[256];
    char* const environment[] = { const_cast<char*>("GREETING=hello"), const_cast<char*>("PATH=/usr/bin:/bin"), nullptr };
    REQUIRE(shell("echo \"$GREETING\"", output, sizeof(output), environment) == 0 && strcmp(output, "hello\n") == 0,
        "posix_spawn(): the program gets the environment given");
    pid_t spawned = 0;
    char const* missing_argv[] = { "missing", nullptr };
    REQUIRE(posix_spawn(&spawned, "/nonexistent", nullptr, nullptr, const_cast<char**>(missing_argv), environ) == ENOENT,
        "posix_spawn() of a missing file returns ENOENT");
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    char const* sleep_argv[] = { "sleep", "5", nullptr };
    REQUIRE(posix_spawnp(&spawned, "sleep", nullptr, &attributes, const_cast<char**>(sleep_argv), environ) == 0, "posix_spawnp() finds a program along PATH");
    struct timespec pause_time { 0, 100'000'000 };
    nanosleep(&pause_time, nullptr);
    REQUIRE(getpgid(spawned) == spawned, "POSIX_SPAWN_SETPGROUP with 0: the child leads a group of its own");
    kill(spawned, SIGKILL);
    waitpid(spawned, nullptr, 0);
    posix_spawnattr_destroy(&attributes);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, path("spawned"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char const* echo_argv[] = { "echo", "into a file", nullptr };
    REQUIRE(posix_spawn(&spawned, "/bin/echo", &actions, nullptr, const_cast<char**>(echo_argv), environ) == 0, "posix_spawn() with an open file action");
    waitpid(spawned, nullptr, 0);
    posix_spawn_file_actions_destroy(&actions);
    int fd = open(path("spawned"), O_RDONLY);
    ssize_t got = read(fd, output, sizeof(output) - 1);
    output[got > 0 ? got : 0] = '\0';
    close(fd);
    REQUIRE(strcmp(output, "into a file\n") == 0, "posix_spawn_file_actions_addopen(): the child's descriptor is the file");

    struct rlimit limit;
    REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur >= 20, "getrlimit(RLIMIT_NOFILE) is at least _POSIX_OPEN_MAX (20)");
    struct rlimit lower = { 32, limit.rlim_max };
    REQUIRE(setrlimit(RLIMIT_NOFILE, &lower) == 0 && getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur == 32, "setrlimit() lowers a soft limit");
    struct rlimit wrong = { limit.rlim_max + 1, limit.rlim_max };
    errno = 0;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &wrong) == -1 && errno == EINVAL, "setrlimit() with the soft limit above the hard: EINVAL");
    end_area();
}

void signals()
{
    begin("signals");
    struct sigaction action {};
    action.sa_handler = count_signal;
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGUSR1, &action, nullptr) == 0, "sigaction() installs a handler");
    s_signal_count = 0;
    raise(SIGUSR1);
    REQUIRE(s_signal_count == 1 && s_last_signal == SIGUSR1, "raise(): the handler runs, with the signal's number");

    struct sigaction old {};
    REQUIRE(sigaction(SIGUSR1, nullptr, &old) == 0 && old.sa_handler == count_signal, "sigaction() reports the handler installed");

    struct sigaction with_info {};
    with_info.sa_sigaction = record_info;
    with_info.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &with_info, nullptr);
    kill(getpid(), SIGUSR2);
    REQUIRE(s_info_signal == SIGUSR2 && s_info_pid == getpid(), "SA_SIGINFO: si_signo, and si_pid of the sender");

    sigset_t block, previous, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    REQUIRE(sigprocmask(SIG_BLOCK, &block, &previous) == 0, "sigprocmask(SIG_BLOCK)");
    s_signal_count = 0;
    raise(SIGUSR1);
    REQUIRE(s_signal_count == 0, "a blocked signal is not delivered");
    REQUIRE(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1), "sigpending() includes it");
    int status = in_child([] {
        sigset_t inherited, child_pending;
        sigprocmask(SIG_BLOCK, nullptr, &inherited);
        sigpending(&child_pending);
        _exit(sigismember(&inherited, SIGUSR1) && !sigismember(&child_pending, SIGUSR1) ? 0 : 1);
    });
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork(): the mask is inherited, pending signals are not");
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    REQUIRE(s_signal_count == 1, "unblocking delivers the pending signal");

    signal(SIGUSR1, SIG_IGN);
    s_signal_count = 0;
    raise(SIGUSR1);
    REQUIRE(s_signal_count == 0, "SIG_IGN: the signal is discarded");
    char output[64];
    REQUIRE(shell("kill -USR1 $$; echo alive", output, sizeof(output)) == 0 && strcmp(output, "alive\n") == 0,
        "exec keeps ignored signals ignored");
    signal(SIGUSR1, count_signal);
    REQUIRE(shell("kill -USR1 $$; echo alive", output, sizeof(output)) == 128 + SIGUSR1, "exec resets caught signals to their default");

    // sigsuspend and alarm.
    sigprocmask(SIG_BLOCK, &block, &previous);
    s_signal_count = 0;
    pid_t child = fork();
    if (child == 0) {
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    sigset_t wait_mask = previous;
    sigdelset(&wait_mask, SIGUSR1);
    errno = 0;
    int result = sigsuspend(&wait_mask);
    REQUIRE(result == -1 && errno == EINTR && s_signal_count == 1, "sigsuspend() returns -1 EINTR after a handler ran");
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    waitpid(child, nullptr, 0);

    signal(SIGALRM, count_signal);
    s_signal_count = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    alarm(1);
    while (s_signal_count == 0 && milliseconds_since(start) < 5000)
        pause();
    REQUIRE(s_signal_count == 1 && s_last_signal == SIGALRM && milliseconds_since(start) >= 900, "alarm(1): SIGALRM after about a second");
    REQUIRE(alarm(10) == 0 && alarm(0) >= 9, "alarm() returns the seconds left of the previous one");

    signal(SIGCHLD, count_signal);
    s_signal_count = 0;
    child = fork();
    if (child == 0)
        _exit(0);
    waitpid(child, nullptr, 0);
    REQUIRE(s_signal_count >= 1 && s_last_signal == SIGCHLD, "SIGCHLD when a child ends");
    signal(SIGCHLD, SIG_DFL);

    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    pipe(fds);
    close(fds[0]);
    errno = 0;
    REQUIRE(write(fds[1], "x", 1) == -1 && errno == EPIPE, "write() to a pipe nobody reads: EPIPE (SIGPIPE ignored)");
    close(fds[1]);
    signal(SIGPIPE, SIG_DFL);
    status = in_child([] {
        int pair[2];
        pipe(pair);
        close(pair[0]);
        (void)write(pair[1], "x", 1);
        _exit(0);
    });
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE, "...and SIGPIPE, which kills by default");
    REQUIRE(strsignal(SIGINT) && *strsignal(SIGINT), "strsignal() names signals");
    end_area();
}

void files()
{
    begin("files");
    char buffer[64];
    int fd = open(path("file"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    REQUIRE(fd >= 0, "open(O_CREAT) makes a file");
    errno = 0;
    REQUIRE(open(path("file"), O_RDWR | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST, "O_CREAT | O_EXCL on a file that exists: EEXIST");
    REQUIRE(write(fd, "hello world", 11) == 11, "write() writes everything to a regular file");
    REQUIRE(lseek(fd, 0, SEEK_CUR) == 11, "the offset follows what was written");
    REQUIRE(lseek(fd, -5, SEEK_END) == 6, "lseek(SEEK_END)");
    REQUIRE(read(fd, buffer, 64) == 5 && memcmp(buffer, "world", 5) == 0, "read() from the offset");
    REQUIRE(read(fd, buffer, 64) == 0, "read() at the end returns 0");
    REQUIRE(pread(fd, buffer, 5, 0) == 5 && memcmp(buffer, "hello", 5) == 0 && lseek(fd, 0, SEEK_CUR) == 11,
        "pread() reads at the offset given and leaves the file's");
    REQUIRE(pwrite(fd, "HELLO", 5, 0) == 5 && lseek(fd, 0, SEEK_CUR) == 11, "pwrite() writes there and leaves the offset");
    REQUIRE(lseek(fd, 20, SEEK_SET) == 20 && write(fd, "!", 1) == 1, "writing past the end");
    REQUIRE(pread(fd, buffer, 9, 11) == 9 && buffer[0] == 0 && buffer[8] == 0 && buffer[0] == buffer[4], "...leaves a gap that reads as zeros");
    errno = 0;
    REQUIRE(lseek(fd, -100, SEEK_SET) == -1 && errno == EINVAL, "lseek() before the start: EINVAL");

    int copy = dup(fd);
    REQUIRE(copy > fd || copy >= 0, "dup() returns the lowest free descriptor");
    lseek(fd, 3, SEEK_SET);
    REQUIRE(lseek(copy, 0, SEEK_CUR) == 3, "duplicates share the offset");
    REQUIRE(dup2(copy, copy) == copy, "dup2(fd, fd) returns fd");
    int high = fcntl(fd, F_DUPFD, 10);
    REQUIRE(high >= 10, "fcntl(F_DUPFD) returns at least the number given");
    REQUIRE(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC && fcntl(copy, F_GETFD) == 0,
        "FD_CLOEXEC belongs to the descriptor, not the open file");
    REQUIRE(fcntl(copy, F_SETFL, O_NONBLOCK) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK), "status flags (O_NONBLOCK) belong to the open file");
    REQUIRE((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR, "F_GETFL reports the access mode");
    close(high);
    close(copy);
    errno = 0;
    REQUIRE(read(copy, buffer, 1) == -1 && errno == EBADF, "a closed descriptor: EBADF");

    // Close-on-exec, across exec.
    int five = dup2(fd, 5);
    REQUIRE(five == 5, "dup2() onto a chosen number");
    fcntl(5, F_SETFD, FD_CLOEXEC);
    REQUIRE(shell("exec 6<&5", nullptr, 0) != 0, "a close-on-exec descriptor is closed in the new program");
    fcntl(5, F_SETFD, 0);
    REQUIRE(shell("exec 6<&5", nullptr, 0) == 0, "...and kept without it");
    close(5);

    struct stat information;
    REQUIRE(ftruncate(fd, 4) == 0 && fstat(fd, &information) == 0 && information.st_size == 4, "ftruncate() shrinks");
    REQUIRE(ftruncate(fd, 4096) == 0 && fstat(fd, &information) == 0 && information.st_size == 4096 && pread(fd, buffer, 1, 4000) == 1 && buffer[0] == 0,
        "ftruncate() grows, with zeros");
    REQUIRE(truncate(path("file"), 2) == 0 && stat(path("file"), &information) == 0 && information.st_size == 2, "truncate() by path");
    REQUIRE(S_ISREG(information.st_mode) && information.st_nlink == 1, "stat(): a regular file with one link");
    close(fd);

    fd = open(path("append"), O_WRONLY | O_CREAT | O_APPEND, 0644);
    write(fd, "one ", 4);
    lseek(fd, 0, SEEK_SET);
    write(fd, "two", 3);
    close(fd);
    fd = open(path("append"), O_RDONLY);
    REQUIRE(read(fd, buffer, 64) == 7 && memcmp(buffer, "one two", 7) == 0, "O_APPEND writes at the end, wherever the offset");
    close(fd);
    fd = open(path("append"), O_WRONLY | O_TRUNC);
    REQUIRE(fd >= 0 && fstat(fd, &information) == 0 && information.st_size == 0, "O_TRUNC empties the file");
    close(fd);

    // Names: links, renames, symbolic links.
    write_file("original", "content");
    REQUIRE(link(path("original"), path("hard")) == 0 && stat(path("original"), &information) == 0 && information.st_nlink == 2,
        "link() adds a name; st_nlink counts it");
    struct stat other;
    stat(path("hard"), &other);
    REQUIRE(other.st_ino == information.st_ino, "both names are one file (st_ino)");
    REQUIRE(unlink(path("hard")) == 0 && stat(path("original"), &information) == 0 && information.st_nlink == 1, "unlink() removes a name");
    fd = open(path("original"), O_RDONLY);
    unlink(path("original"));
    REQUIRE(fstat(fd, &information) == 0 && information.st_nlink == 0 && read(fd, buffer, 7) == 7, "an unlinked file lives while open");
    close(fd);
    errno = 0;
    REQUIRE(stat(path("original"), &information) == -1 && errno == ENOENT, "...but has no name: ENOENT");

    write_file("from", "new");
    write_file("to", "old");
    REQUIRE(rename(path("from"), path("to")) == 0, "rename() over a file that exists");
    fd = open(path("to"), O_RDONLY);
    REQUIRE(read(fd, buffer, 3) == 3 && memcmp(buffer, "new", 3) == 0 && access(path("from"), F_OK) == -1, "...replaces it");
    close(fd);
    mkdir(path("parent"), 0755);
    mkdir(path("parent/child"), 0755);
    errno = 0;
    REQUIRE(rename(path("parent"), path("parent/child/inside")) == -1 && errno == EINVAL, "rename() of a directory into itself: EINVAL");

    REQUIRE(symlink("to", path("link")) == 0, "symlink()");
    ssize_t length = readlink(path("link"), buffer, sizeof(buffer));
    REQUIRE(length == 2 && memcmp(buffer, "to", 2) == 0, "readlink() gives the target, not NUL-terminated");
    REQUIRE(lstat(path("link"), &information) == 0 && S_ISLNK(information.st_mode), "lstat() describes the link");
    REQUIRE(stat(path("link"), &information) == 0 && S_ISREG(information.st_mode), "stat() follows it");
    errno = 0;
    REQUIRE(open(path("link"), O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP, "O_NOFOLLOW on a symbolic link: ELOOP");
    errno = 0;
    REQUIRE(readlink(path("to"), buffer, sizeof(buffer)) == -1 && errno == EINVAL, "readlink() of something else: EINVAL");

    // Permissions.
    mode_t old_mask = umask(077);
    write_file("private", "x");
    REQUIRE(umask(old_mask) == 077, "umask() returns the previous mask");
    REQUIRE(stat(path("private"), &information) == 0 && (information.st_mode & 0777) == 0600, "the mask takes bits from new files' modes");
    REQUIRE(chmod(path("private"), 0750) == 0 && stat(path("private"), &information) == 0 && (information.st_mode & 0777) == 0750, "chmod()");
    REQUIRE(access(path("private"), R_OK | W_OK) == 0, "access(R_OK | W_OK)");

    // Timestamps.
    struct timespec times[2] = { { 1000000000, 0 }, { 1500000000, 0 } };
    REQUIRE(utimensat(AT_FDCWD, path("private"), times, 0) == 0 && stat(path("private"), &information) == 0 && information.st_mtime == 1500000000
            && information.st_atime == 1000000000,
        "utimensat() sets the access and modification times");
    time_t before = time(nullptr);
    fd = open(path("private"), O_WRONLY);
    write(fd, "y", 1);
    close(fd);
    REQUIRE(stat(path("private"), &information) == 0 && information.st_mtime >= before, "write() updates st_mtime");

    // Names too long.
    char long_name[300];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    errno = 0;
    REQUIRE(open(long_name, O_RDONLY | O_CREAT, 0644) == -1 && errno == ENAMETOOLONG, "a name longer than NAME_MAX: ENAMETOOLONG");
    end_area();
}

void directories()
{
    begin("directories");
    REQUIRE(mkdir(path("d"), 0755) == 0, "mkdir()");
    errno = 0;
    REQUIRE(mkdir(path("d"), 0755) == -1 && errno == EEXIST, "mkdir() of a name that exists: EEXIST");
    write_file("d/a", "");
    write_file("d/b", "");
    errno = 0;
    REQUIRE(rmdir(path("d")) == -1 && (errno == ENOTEMPTY || errno == EEXIST), "rmdir() of a directory with entries: ENOTEMPTY");
    errno = 0;
    REQUIRE(rmdir(path("d/a")) == -1 && errno == ENOTDIR, "rmdir() of a file: ENOTDIR");
    errno = 0;
    REQUIRE(unlink(path("d")) == -1 && (errno == EISDIR || errno == EPERM), "unlink() of a directory: EISDIR or EPERM");
    errno = 0;
    REQUIRE(open(path("d"), O_WRONLY) == -1 && errno == EISDIR, "open() of a directory for writing: EISDIR");
    errno = 0;
    REQUIRE(open(path("d/a"), O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR, "O_DIRECTORY on a file: ENOTDIR");

    DIR* listing = opendir(path("d"));
    REQUIRE(listing != nullptr, "opendir()");
    bool dot = false, dot_dot = false, a = false, b = false;
    int entries = 0;
    while (struct dirent* entry = readdir(listing)) {
        ++entries;
        dot = dot || strcmp(entry->d_name, ".") == 0;
        dot_dot = dot_dot || strcmp(entry->d_name, "..") == 0;
        a = a || strcmp(entry->d_name, "a") == 0;
        b = b || strcmp(entry->d_name, "b") == 0;
    }
    REQUIRE(dot && dot_dot && a && b && entries == 4, "readdir() gives every entry, \".\" and \"..\" included, once");
    rewinddir(listing);
    REQUIRE(readdir(listing) != nullptr, "rewinddir() starts over");
    closedir(listing);

    char cwd[256];
    char original[256];
    getcwd(original, sizeof(original));
    REQUIRE(chdir(path("d")) == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, path("d")) == 0, "chdir(); getcwd() reports it");
    REQUIRE(access("a", F_OK) == 0, "relative names start at the working directory");
    errno = 0;
    REQUIRE(chdir("a") == -1 && errno == ENOTDIR, "chdir() to a file: ENOTDIR");
    errno = 0;
    REQUIRE(getcwd(cwd, 3) == nullptr && errno == ERANGE, "getcwd() into a buffer too small: ERANGE");
    chdir(original);
    unlink(path("d/a"));
    unlink(path("d/b"));
    REQUIRE(rmdir(path("d")) == 0, "rmdir() of an empty directory");
    end_area();
}

void pipes()
{
    begin("pipes");
    int fds[2];
    REQUIRE(pipe(fds) == 0, "pipe()");
    write(fds[1], "first ", 6);
    write(fds[1], "second", 6);
    char buffer[32] = {};
    REQUIRE(read(fds[0], buffer, sizeof(buffer)) == 12 && strcmp(buffer, "first second") == 0, "data comes out in the order it went in");
    struct pollfd poller { fds[0], POLLIN, 0 };
    REQUIRE(poll(&poller, 1, 0) == 0, "poll(): an empty pipe is not readable");
    write(fds[1], "x", 1);
    REQUIRE(poll(&poller, 1, 0) == 1 && (poller.revents & POLLIN), "poll(): POLLIN once there is data");
    read(fds[0], buffer, 1);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    errno = 0;
    REQUIRE(read(fds[0], buffer, 1) == -1 && errno == EAGAIN, "O_NONBLOCK: reading an empty pipe gives EAGAIN");
    close(fds[1]);
    REQUIRE(read(fds[0], buffer, 1) == 0, "with every writer gone, read() returns 0");
    poller.revents = 0;
    REQUIRE(poll(&poller, 1, 0) == 1 && (poller.revents & (POLLHUP | POLLIN)), "poll() reports the hangup");
    close(fds[0]);
    errno = 0;
    REQUIRE(lseek(fds[0], 0, SEEK_SET) == -1 && errno == EBADF, "lseek() on a closed descriptor: EBADF");
    pipe(fds);
    errno = 0;
    REQUIRE(lseek(fds[0], 0, SEEK_SET) == -1 && errno == ESPIPE, "lseek() on a pipe: ESPIPE");
    errno = 0;
    REQUIRE(!isatty(fds[0]) && errno == ENOTTY, "isatty() of a pipe: 0, ENOTTY");
    close(fds[0]);
    close(fds[1]);
    end_area();
}

void terminals()
{
    begin("terminals");
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master >= 0, "posix_openpt()");
    REQUIRE(grantpt(master) == 0 && unlockpt(master) == 0, "grantpt(), unlockpt()");
    char const* name = ptsname(master);
    REQUIRE(name && strncmp(name, "/dev/", 5) == 0, "ptsname() names the other end");
    int slave = open(name, O_RDWR | O_NOCTTY);
    REQUIRE(slave >= 0 && isatty(slave), "the other end is a terminal");
    char const* tty = ttyname(slave);
    REQUIRE(tty && strcmp(tty, name) == 0, "ttyname() gives its name");
    struct termios settings;
    REQUIRE(tcgetattr(slave, &settings) == 0 && (settings.c_lflag & ICANON) && (settings.c_lflag & ECHO), "a new terminal is canonical, with echo");
    settings.c_lflag &= ~(ICANON | ECHO);
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    struct termios check;
    REQUIRE(tcsetattr(slave, TCSANOW, &settings) == 0 && tcgetattr(slave, &check) == 0 && !(check.c_lflag & ICANON), "tcsetattr() changes the modes");
    write(master, "k", 1);
    char key = 0;
    REQUIRE(read(slave, &key, 1) == 1 && key == 'k', "non-canonical: a byte is readable at once");
    settings.c_lflag |= ICANON;
    tcsetattr(slave, TCSANOW, &settings);
    write(master, "line\n", 5);
    char line[16] = {};
    REQUIRE(read(slave, line, sizeof(line)) == 5 && strcmp(line, "line\n") == 0, "canonical: a whole line");
    REQUIRE(cfgetospeed(&settings) == cfgetispeed(&settings), "cfgetispeed(), cfgetospeed()");
    REQUIRE(tcflush(slave, TCIOFLUSH) == 0 && tcdrain(slave) == 0, "tcflush(), tcdrain()");
    errno = 0;
    REQUIRE(tcgetattr(STDIN_FILENO, &settings) == 0 || errno == ENOTTY, "tcgetattr() of a non-terminal: ENOTTY");
    close(slave);
    close(master);
    end_area();
}

void times()
{
    begin("time");
    struct timespec real, monotonic, later;
    REQUIRE(clock_gettime(CLOCK_REALTIME, &real) == 0 && real.tv_sec > 1600000000, "CLOCK_REALTIME tells the date");
    REQUIRE(time(nullptr) - real.tv_sec <= 1, "time() agrees with it");
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0, "CLOCK_MONOTONIC");
    struct timespec nap { 0, 50'000'000 };
    REQUIRE(nanosleep(&nap, nullptr) == 0, "nanosleep()");
    clock_gettime(CLOCK_MONOTONIC, &later);
    long long elapsed = (later.tv_sec - monotonic.tv_sec) * 1000LL + (later.tv_nsec - monotonic.tv_nsec) / 1'000'000;
    REQUIRE(elapsed >= 50, "nanosleep() sleeps at least as long as asked");
    struct timespec resolution;
    REQUIRE(clock_getres(CLOCK_MONOTONIC, &resolution) == 0 && resolution.tv_sec == 0 && resolution.tv_nsec > 0, "clock_getres()");
    errno = 0;
    struct timespec negative { -1, 0 };
    REQUIRE(nanosleep(&negative, nullptr) == -1 && errno == EINVAL, "nanosleep() of a negative time: EINVAL");

    time_t epoch = 0;
    struct tm broken;
    gmtime_r(&epoch, &broken);
    REQUIRE(broken.tm_year == 70 && broken.tm_mon == 0 && broken.tm_mday == 1 && broken.tm_wday == 4 && broken.tm_hour == 0, "gmtime(0): Thursday 1 January 1970");
    time_t moment = 1700000000;
    gmtime_r(&moment, &broken);
    REQUIRE(timegm(&broken) == moment, "timegm() undoes gmtime()");
    char text[64];
    REQUIRE(strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %a %b %j", &broken) > 0 && strcmp(text, "2023-11-14 22:13:20 Tue Nov 318") == 0, "strftime()");
    struct tm parsed {};
    char const* rest = strptime("2023-11-14 22:13:20", "%Y-%m-%d %H:%M:%S", &parsed);
    REQUIRE(rest && *rest == '\0' && timegm(&parsed) == moment, "strptime() reads what strftime() wrote");
    REQUIRE(strptime("Tuesday, 14 Nov 23 10:13:20 PM", "%A, %d %b %y %I:%M:%S %p", &parsed) && parsed.tm_hour == 22 && parsed.tm_year == 123,
        "strptime() with names, a two-digit year and %p");
    REQUIRE(strptime("not a date", "%Y", &parsed) == nullptr, "strptime() of text that does not match: NULL");
    REQUIRE(difftime(10, 4) == 6.0, "difftime()");
    struct tm local;
    REQUIRE(localtime_r(&moment, &local) && mktime(&local) == moment, "mktime() undoes localtime()");
    end_area();
}

void memory()
{
    begin("memory");
    REQUIRE(sysconf(_SC_PAGESIZE) == 4096, "sysconf(_SC_PAGESIZE)");
    auto* page = static_cast<unsigned char*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(page != MAP_FAILED, "mmap(MAP_ANONYMOUS)");
    bool zero = true;
    for (int i = 0; i < 8192; ++i)
        zero = zero && page[i] == 0;
    REQUIRE(zero, "anonymous memory starts zeroed");
    page[100] = 7;
    int status = in_child([&] { _exit(page[100] == 7 ? 0 : 1); });
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a private mapping is inherited by fork()");
    REQUIRE(mprotect(page, 4096, PROT_READ) == 0, "mprotect()");
    status = in_child([&] { page[0] = 1; });
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "writing to read-only memory: SIGSEGV");
    REQUIRE(munmap(page, 8192) == 0, "munmap()");
    status = in_child([&] { _exit(page[4096]); });
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "touching unmapped memory: SIGSEGV");
    errno = 0;
    REQUIRE(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED, "writable and executable at once is refused");
    errno = 0;
    REQUIRE(mmap(nullptr, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL, "mmap() of nothing: EINVAL");

    int fd = open(path("mapped"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    ftruncate(fd, 4096);
    auto* shared = static_cast<char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    REQUIRE(shared != MAP_FAILED, "mmap(MAP_SHARED) of a file");
    memcpy(shared, "through memory", 14);
    REQUIRE(msync(shared, 4096, MS_SYNC) == 0, "msync()");
    char buffer[16] = {};
    REQUIRE(pread(fd, buffer, 14, 0) == 14 && memcmp(buffer, "through memory", 14) == 0, "a shared mapping's writes are the file's");
    auto* private_map = static_cast<char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0));
    private_map[0] = 'X';
    REQUIRE(pread(fd, buffer, 1, 0) == 1 && buffer[0] == 't', "a private mapping's writes are its own");
    REQUIRE(madvise(shared, 4096, MADV_SEQUENTIAL) == 0, "madvise()");
    munmap(shared, 4096);
    munmap(private_map, 4096);
    close(fd);
    end_area();
}

void environment()
{
    begin("environment");
    REQUIRE(setenv("POSIX_TEST", "one", 1) == 0 && strcmp(getenv("POSIX_TEST"), "one") == 0, "setenv(), getenv()");
    REQUIRE(setenv("POSIX_TEST", "two", 0) == 0 && strcmp(getenv("POSIX_TEST"), "one") == 0, "setenv() without overwrite keeps the value");
    bool listed = false;
    for (char** entry = environ; *entry; ++entry)
        listed = listed || strcmp(*entry, "POSIX_TEST=one") == 0;
    REQUIRE(listed, "environ lists it");
    char output[64];
    REQUIRE(shell("echo $POSIX_TEST", output, sizeof(output)) == 0 && strcmp(output, "one\n") == 0, "children get the environment");
    REQUIRE(unsetenv("POSIX_TEST") == 0 && getenv("POSIX_TEST") == nullptr, "unsetenv()");
    errno = 0;
    REQUIRE(setenv("A=B", "x", 1) == -1 && errno == EINVAL, "setenv() of a name with '=': EINVAL");

    REQUIRE(getuid() == 0 && geteuid() == 0, "the tests run as root");
    struct passwd* root = getpwuid(0);
    REQUIRE(root && strcmp(root->pw_name, "root") == 0 && root->pw_uid == 0, "getpwuid(0): root");
    struct passwd* user = getpwnam("vulture");
    REQUIRE(user && user->pw_uid == 501 && strcmp(user->pw_dir, "/Users/vulture") == 0, "getpwnam()");
    REQUIRE(getpwnam("no-such-user") == nullptr, "getpwnam() of nobody known: NULL");
    struct group* wheel = getgrgid(0);
    REQUIRE(wheel && strcmp(wheel->gr_name, "wheel") == 0, "getgrgid(0): wheel");
    struct group* staff = getgrnam("staff");
    REQUIRE(staff && staff->gr_gid == 20, "getgrnam()");

    struct utsname name;
    REQUIRE(uname(&name) == 0 && strcmp(name.sysname, "vultureOS") == 0 && strcmp(name.machine, NATIVE_MACHINE) == 0, "uname()");
    char host[256];
    REQUIRE(gethostname(host, sizeof(host)) == 0 && strcmp(host, name.nodename) == 0, "gethostname() is uname()'s nodename");
    REQUIRE(sysconf(_SC_NPROCESSORS_ONLN) >= 1 && sysconf(_SC_OPEN_MAX) >= 20 && sysconf(_SC_ARG_MAX) >= 4096, "sysconf() limits");
    REQUIRE(getpid() > 1 && getppid() >= 1, "getpid(), getppid()");
    end_area();
}

int compare_ints(void const* a, void const* b)
{
    return *static_cast<int const*>(a) - *static_cast<int const*>(b);
}

int s_exit_fd = -1;

void library()
{
    begin("library");
    char text[64];
    snprintf(text, sizeof(text), "%d|%5.2f|%-4s|%x|%o|%c|%%|%05d|%+d|%lld", -42, 3.14159, "ab", 255, 8, 'z', 42, 7, 1LL << 40);
    REQUIRE(strcmp(text, "-42| 3.14|ab  |ff|10|z|%|00042|+7|1099511627776") == 0, "printf() conversions and flags");
    REQUIRE(snprintf(text, 4, "%s", "truncated") == 9 && strcmp(text, "tru") == 0, "snprintf() truncates and returns the full length");
    char* end = nullptr;
    REQUIRE(strtol("0x1F", &end, 0) == 31 && *end == '\0', "strtol() base 0: hexadecimal");
    REQUIRE(strtol("017", nullptr, 0) == 15, "strtol() base 0: octal");
    REQUIRE(strtol("  -12abc", &end, 10) == -12 && strcmp(end, "abc") == 0, "strtol() skips space and stops at the first non-digit");
    errno = 0;
    REQUIRE(strtol("99999999999999999999", nullptr, 10) == LONG_MAX && errno == ERANGE, "strtol() overflow: LONG_MAX, ERANGE");
    REQUIRE(strtoul("ff", nullptr, 16) == 255, "strtoul()");
    REQUIRE(strtod("2.5e3", nullptr) == 2500.0, "strtod()");
    REQUIRE(atoi("123") == 123, "atoi()");
    int numbers[] = { 5, 3, 9, 1, 7 };
    qsort(numbers, 5, sizeof(int), compare_ints);
    REQUIRE(numbers[0] == 1 && numbers[4] == 9, "qsort()");
    int key = 7;
    REQUIRE(bsearch(&key, numbers, 5, sizeof(int), compare_ints) == &numbers[3], "bsearch()");
    char overlap[] = "abcdef";
    memmove(overlap + 2, overlap, 4);
    REQUIRE(strcmp(overlap, "ababcd") == 0, "memmove() of overlapping ranges");
    char padded[8];
    strncpy(padded, "ab", sizeof(padded));
    REQUIRE(padded[2] == '\0' && padded[7] == '\0', "strncpy() pads with NULs");
    char tokens[] = "a,b,,c";
    char* state = nullptr;
    char* first = strtok_r(tokens, ",", &state);
    char* second = strtok_r(nullptr, ",", &state);
    char* third = strtok_r(nullptr, ",", &state);
    REQUIRE(first && second && third && strcmp(first, "a") == 0 && strcmp(second, "b") == 0 && strcmp(third, "c") == 0 && !strtok_r(nullptr, ",", &state),
        "strtok_r() skips empty fields");
    REQUIRE(strerror(EINVAL) && *strerror(EINVAL), "strerror() names errors");
    int number = 0;
    char word[16];
    REQUIRE(sscanf("42 answer", "%d %15s", &number, word) == 2 && number == 42 && strcmp(word, "answer") == 0, "sscanf()");

    FILE* file = fopen(path("stdio"), "w+");
    REQUIRE(file != nullptr, "fopen(\"w+\")");
    fprintf(file, "line one\nline two\n");
    REQUIRE(ftell(file) == 18, "ftell()");
    rewind(file);
    char line[32];
    REQUIRE(fgets(line, sizeof(line), file) && strcmp(line, "line one\n") == 0, "fgets() reads a line, newline included");
    int c = fgetc(file);
    REQUIRE(ungetc(c, file) == c && fgetc(file) == c, "ungetc() puts a character back");
    REQUIRE(fseek(file, 0, SEEK_END) == 0 && ftell(file) == 18, "fseek(SEEK_END)");
    REQUIRE(fgetc(file) == EOF && feof(file), "EOF at the end, and feof()");
    fclose(file);
    FILE* temporary = tmpfile();
    REQUIRE(temporary && fputs("x", temporary) >= 0, "tmpfile()");
    if (temporary)
        fclose(temporary);

    // atexit: handlers run in reverse order.
    int fds[2];
    pipe(fds);
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        s_exit_fd = fds[1];
        atexit([] { (void)write(s_exit_fd, "1", 1); });
        atexit([] { (void)write(s_exit_fd, "2", 1); });
        exit(0);
    }
    close(fds[1]);
    char order[4] = {};
    size_t got_bytes = 0;
    ssize_t got;
    while (got_bytes < 3 && (got = read(fds[0], order + got_bytes, 3 - got_bytes)) > 0)
        got_bytes += static_cast<size_t>(got);
    close(fds[0]);
    waitpid(child, nullptr, 0);
    REQUIRE(strcmp(order, "21") == 0, "atexit() handlers run in reverse order");

    char const* arguments[] = { "program", "-a", "-b", "value", "rest", nullptr };
    optind = 1;
    int option;
    bool saw_a = false;
    char const* b_value = nullptr;
    while ((option = getopt(5, const_cast<char**>(arguments), "ab:")) != -1) {
        if (option == 'a')
            saw_a = true;
        else if (option == 'b')
            b_value = optarg;
    }
    REQUIRE(saw_a && b_value && strcmp(b_value, "value") == 0 && optind == 4, "getopt()");

    regex_t expression;
    regmatch_t matches[3];
    REQUIRE(regcomp(&expression, "([a-z]+)=([0-9]+)", REG_EXTENDED) == 0, "regcomp()");
    REQUIRE(regexec(&expression, "x key=42 y", 3, matches, 0) == 0 && matches[1].rm_so == 2 && matches[1].rm_eo == 5 && matches[2].rm_so == 6 && matches[2].rm_eo == 8,
        "regexec() with subexpressions");
    REQUIRE(regexec(&expression, "no match here", 0, nullptr, 0) == REG_NOMATCH, "regexec() without a match: REG_NOMATCH");
    regfree(&expression);
    REQUIRE(fnmatch("*.c", "main.c", 0) == 0 && fnmatch("*.c", "main.h", 0) == FNM_NOMATCH && fnmatch("[a-c]?", "b1", 0) == 0, "fnmatch()");
    write_file("one.txt", "");
    write_file("two.txt", "");
    glob_t found;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s/*.txt", s_dir);
    REQUIRE(glob(pattern, 0, nullptr, &found) == 0 && found.gl_pathc == 2 && strstr(found.gl_pathv[0], "one.txt"), "glob(), sorted");
    globfree(&found);
    end_area();
}

}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    system("rm -rf /tmp/posix");
    mkdir(s_dir, 0755);
    processes();
    signals();
    files();
    directories();
    pipes();
    terminals();
    times();
    memory();
    environment();
    library();
    system("rm -rf /tmp/posix");
    printf("posix: %d of %d requirements met: %s\n", s_total_passed, s_total_passed + s_total_failed, s_total_failed ? "FAIL" : "PASS");
    return s_total_failed ? 1 : 0;
}
