/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// More of posix.cpp: what real programs want of a system, area by area. These run on a real Mac too (Meta/oracle-test.sh), and
// each difference between the two has to be explained, so this says what's expected of both, not what's convenient for either.

void at_calls()
{
    begin("at calls");
    mkdir(path("at"), 0755);
    int directory = open(path("at"), O_RDONLY | O_DIRECTORY);
    REQUIRE(directory >= 0, "open(O_DIRECTORY) of a directory");
    int not_directory = open("/etc/passwd", O_RDONLY | O_DIRECTORY);
    REQUIRE(not_directory < 0 && errno == ENOTDIR, "open(O_DIRECTORY) of a file: ENOTDIR");

    int fd = openat(directory, "made", O_WRONLY | O_CREAT | O_TRUNC, 0640);
    REQUIRE(fd >= 0, "openat() makes a file in that directory");
    REQUIRE(write(fd, "abc", 3) == 3, "write() to what openat() opened");
    close(fd);
    struct stat status;
    REQUIRE(stat(path("at/made"), &status) == 0 && status.st_size == 3, "openat()'s file is where it should be");
    REQUIRE(fstatat(directory, "made", &status, 0) == 0 && status.st_size == 3, "fstatat() of a name in a directory");
    REQUIRE((status.st_mode & 0777) == 0640, "fstatat() has the mode openat() asked for");
    errno = 0;
    REQUIRE(fstatat(directory, "absent", &status, 0) < 0 && errno == ENOENT, "fstatat() of nothing: ENOENT");
    REQUIRE(fstatat(AT_FDCWD, path("at/made"), &status, 0) == 0, "fstatat(AT_FDCWD) of an absolute path");
    REQUIRE(fstatat(-1, path("at/made"), &status, 0) == 0, "fstatat() with an absolute path ignores the descriptor");
    errno = 0;
    REQUIRE(fstatat(-1, "made", &status, 0) < 0 && errno == EBADF, "fstatat() of a relative path with no directory: EBADF");
    int a_file = open(path("at/made"), O_RDONLY);
    errno = 0;
    REQUIRE(fstatat(a_file, "made", &status, 0) < 0 && errno == ENOTDIR, "fstatat() relative to a file: ENOTDIR");
    close(a_file);

    REQUIRE(mkdirat(directory, "sub", 0750) == 0, "mkdirat()");
    REQUIRE(fstatat(directory, "sub", &status, 0) == 0 && S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0750, "mkdirat() made a directory, with the mode");
    errno = 0;
    REQUIRE(mkdirat(directory, "sub", 0750) < 0 && errno == EEXIST, "mkdirat() of what's there: EEXIST");

    REQUIRE(symlinkat("made", directory, "link") == 0, "symlinkat()");
    char target[64] = {};
    REQUIRE(readlinkat(directory, "link", target, sizeof(target)) == 4 && memcmp(target, "made", 4) == 0, "readlinkat()");
    REQUIRE(fstatat(directory, "link", &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(status.st_mode), "fstatat(AT_SYMLINK_NOFOLLOW) sees the link");
    REQUIRE(fstatat(directory, "link", &status, 0) == 0 && S_ISREG(status.st_mode), "fstatat() goes through the link");
    errno = 0;
    REQUIRE(readlinkat(directory, "made", target, sizeof(target)) < 0 && errno == EINVAL, "readlinkat() of a file: EINVAL");

    REQUIRE(linkat(directory, "made", directory, "second", 0) == 0, "linkat()");
    REQUIRE(fstatat(directory, "second", &status, 0) == 0 && status.st_nlink == 2, "linkat() makes another name");
    REQUIRE(unlinkat(directory, "second", 0) == 0, "unlinkat() of a file");
    REQUIRE(fstatat(directory, "made", &status, 0) == 0 && status.st_nlink == 1, "unlinkat() leaves the other name");

    REQUIRE(renameat(directory, "made", directory, "moved") == 0, "renameat()");
    REQUIRE(faccessat(directory, "moved", R_OK, 0) == 0, "faccessat() of what was renamed");
    errno = 0;
    REQUIRE(faccessat(directory, "made", F_OK, 0) < 0 && errno == ENOENT, "faccessat() of the old name: ENOENT");
    REQUIRE(renameat(AT_FDCWD, path("at/moved"), directory, "back") == 0, "renameat() between the two kinds of path");

    REQUIRE(fchmodat(directory, "back", 0600, 0) == 0, "fchmodat()");
    REQUIRE(fstatat(directory, "back", &status, 0) == 0 && (status.st_mode & 0777) == 0600, "fchmodat() changed it");
    REQUIRE(fchownat(directory, "back", static_cast<uid_t>(-1), static_cast<gid_t>(-1), 0) == 0, "fchownat() changing nothing");

    timespec times[2];
    times[0].tv_sec = 1000;
    times[0].tv_nsec = 0;
    times[1].tv_sec = 2000;
    times[1].tv_nsec = 0;
    REQUIRE(utimensat(directory, "back", times, 0) == 0, "utimensat() relative to a directory");
    REQUIRE(fstatat(directory, "back", &status, 0) == 0 && status.st_mtime == 2000, "utimensat() set the time");

    errno = 0;
    REQUIRE(unlinkat(directory, "sub", 0) < 0 && (errno == EPERM || errno == EISDIR), "unlinkat() of a directory without the flag");
    REQUIRE(unlinkat(directory, "sub", AT_REMOVEDIR) == 0, "unlinkat(AT_REMOVEDIR)");
    errno = 0;
    REQUIRE(unlinkat(directory, "back", AT_REMOVEDIR) < 0 && errno == ENOTDIR, "unlinkat(AT_REMOVEDIR) of a file: ENOTDIR");

    char was[1024];
    REQUIRE(getcwd(was, sizeof(was)) != nullptr, "getcwd()");
    REQUIRE(fchdir(directory) == 0, "fchdir()");
    REQUIRE(stat("back", &status) == 0, "after fchdir(), a relative path starts there");
    char here[1024];
    REQUIRE(getcwd(here, sizeof(here)) != nullptr && strstr(here, "posix/at") != nullptr, "after fchdir(), getcwd() says so");
    REQUIRE(chdir(was) == 0, "chdir() back");
    int plain = open(path("at/back"), O_RDONLY);
    errno = 0;
    REQUIRE(fchdir(plain) < 0 && errno == ENOTDIR, "fchdir() to a file: ENOTDIR");
    close(plain);
    errno = 0;
    REQUIRE(fchdir(9999) < 0 && errno == EBADF, "fchdir() of nothing open: EBADF");
    char from_descriptor[1024] = {};
    REQUIRE(fcntl(directory, F_GETPATH, from_descriptor) == 0 && strstr(from_descriptor, "posix/at") != nullptr, "fcntl(F_GETPATH)");
    close(directory);
    end_area();
}

void descriptors()
{
    begin("descriptors");
    rlimit limit {};
    REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0, "getrlimit(RLIMIT_NOFILE)");
    // (As the tests before this one left it: see what it's like with plenty.)
    limit.rlim_cur = 256;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &limit) == 0, "setrlimit() to 256");
    REQUIRE(sysconf(_SC_OPEN_MAX) == static_cast<long>(limit.rlim_cur), "sysconf(_SC_OPEN_MAX) is the limit");
    REQUIRE(getdtablesize() == static_cast<int>(limit.rlim_cur), "getdtablesize() is the limit");
    rlimit const was = limit;

    rlimit lower = limit;
    lower.rlim_cur = 32;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &lower) == 0, "setrlimit() lowers the limit");
    int fds[64];
    int opened = 0;
    int const first_free = dup(0);
    close(first_free);
    while (opened < 64) {
        fds[opened] = open("/dev/null", O_RDONLY);
        if (fds[opened] < 0)
            break;
        ++opened;
    }
    REQUIRE(errno == EMFILE && opened > 0 && opened < 32, "past the limit: EMFILE");
    for (int i = 0; i < opened; ++i)
        close(fds[i]);
    errno = 0;
    REQUIRE(dup2(0, 40) < 0 && errno == EBADF, "dup2() past the limit: EBADF");
    errno = 0;
    REQUIRE(fcntl(0, F_DUPFD, 40) < 0 && errno == EINVAL, "fcntl(F_DUPFD) past the limit: EINVAL");
    REQUIRE(setrlimit(RLIMIT_NOFILE, &was) == 0, "setrlimit() puts the limit back");

    rlimit big = was;
    big.rlim_cur = 4000;
    if (big.rlim_max < big.rlim_cur)
        big.rlim_cur = big.rlim_max;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &big) == 0, "setrlimit() raises the limit");
    int const high = dup2(0, 3000);
    REQUIRE(high == 3000, "dup2() to a high descriptor");
    REQUIRE(fcntl(3000, F_GETFD) == 0, "a high descriptor is open");
    int const after = fcntl(0, F_DUPFD, 2500);
    REQUIRE(after == 2500, "fcntl(F_DUPFD) finds the lowest from where it's asked");
    int const next = fcntl(0, F_DUPFD, 2500);
    REQUIRE(next == 2501, "fcntl(F_DUPFD) goes to the next one");
    int const with_flag = fcntl(0, F_DUPFD_CLOEXEC, 2600);
    REQUIRE(with_flag == 2600 && (fcntl(with_flag, F_GETFD) & FD_CLOEXEC), "fcntl(F_DUPFD_CLOEXEC)");
    close(3000);
    close(2500);
    close(2501);
    close(2600);
    errno = 0;
    REQUIRE(fcntl(3000, F_GETFD) < 0 && errno == EBADF, "closed: EBADF");

    // There's a limit on what can be open, whatever this one says.
    rlimit unlimited = was;
    unlimited.rlim_cur = RLIM_INFINITY;
    if (unlimited.rlim_max == RLIM_INFINITY) {
        REQUIRE(setrlimit(RLIMIT_NOFILE, &unlimited) == 0, "setrlimit() of no limit");
        rlimit seen {};
        REQUIRE(getrlimit(RLIMIT_NOFILE, &seen) == 0 && seen.rlim_cur == RLIM_INFINITY, "getrlimit() says so");
        REQUIRE(sysconf(_SC_OPEN_MAX) == static_cast<long>(RLIM_INFINITY), "sysconf(_SC_OPEN_MAX) says so");
        (void)setrlimit(RLIMIT_NOFILE, &was);
    }
    errno = 0;
    REQUIRE(getrlimit(9999, &limit) < 0 && errno == EINVAL, "getrlimit() of no such limit: EINVAL");
    rlimit backwards {};
    backwards.rlim_cur = 100;
    backwards.rlim_max = 10;
    errno = 0;
    REQUIRE(setrlimit(RLIMIT_CORE, &backwards) < 0 && errno == EINVAL, "setrlimit() with the soft limit over the hard: EINVAL");
    (void)setrlimit(RLIMIT_NOFILE, &was);

    int child_sees = -1;
    rlimit inherited = was;
    inherited.rlim_cur = 100;
    (void)setrlimit(RLIMIT_NOFILE, &inherited);
    int status_pipe[2];
    (void)pipe(status_pipe);
    pid_t child = fork();
    if (child == 0) {
        rlimit seen {};
        getrlimit(RLIMIT_NOFILE, &seen);
        int const value = static_cast<int>(seen.rlim_cur);
        (void)write(status_pipe[1], &value, sizeof(value));
        _exit(0);
    }
    (void)read(status_pipe[0], &child_sees, sizeof(child_sees));
    waitpid(child, nullptr, 0);
    close(status_pipe[0]);
    close(status_pipe[1]);
    REQUIRE(child_sees == 100, "fork() keeps limits");
    (void)setrlimit(RLIMIT_NOFILE, &was);

    int const source = open("/dev/null", O_RDONLY);
    int const copy = dup(source);
    fcntl(source, F_SETFL, O_NONBLOCK);
    REQUIRE(fcntl(copy, F_GETFL) & O_NONBLOCK, "dup()'s copy shares the status flags");
    REQUIRE(!(fcntl(copy, F_GETFD) & FD_CLOEXEC), "dup()'s copy doesn't get close-on-exec");
    fcntl(source, F_SETFD, FD_CLOEXEC);
    REQUIRE(!(fcntl(copy, F_GETFD) & FD_CLOEXEC), "close-on-exec is the descriptor's own");
    close(source);
    close(copy);
    errno = 0;
    REQUIRE(close(9999) < 0 && errno == EBADF, "close() of nothing: EBADF");
    errno = 0;
    REQUIRE(read(9999, fds, 1) < 0 && errno == EBADF, "read() of nothing: EBADF");
    errno = 0;
    REQUIRE(dup(9999) < 0 && errno == EBADF, "dup() of nothing: EBADF");
    errno = 0;
    REQUIRE(fcntl(0, 9999) < 0 && errno == EINVAL, "fcntl() of no such command: EINVAL");
    end_area();
}

void vectored_io()
{
    begin("vectored io");
    char one[4] = "abc";
    char two[4] = "def";
    iovec out[2] = { { one, 3 }, { two, 3 } };
    int fd = open(path("vectors"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    REQUIRE(writev(fd, out, 2) == 6, "writev()");
    REQUIRE(lseek(fd, 0, SEEK_CUR) == 6, "writev() moves the offset");
    lseek(fd, 0, SEEK_SET);
    char first[2] = {}, second[4] = {}, third[8] = {};
    iovec in[3] = { { first, 2 }, { second, 4 }, { third, 8 } };
    REQUIRE(readv(fd, in, 3) == 6, "readv() of less than all of it");
    REQUIRE(memcmp(first, "ab", 2) == 0 && memcmp(second, "cdef", 4) == 0 && third[0] == 0, "readv() fills each in turn");
    REQUIRE(readv(fd, in, 3) == 0, "readv() at the end: 0");
    errno = 0;
    REQUIRE(readv(fd, in, 0) < 0 && errno == EINVAL, "readv() of no buffers: EINVAL");
    errno = 0;
    REQUIRE(writev(fd, out, -1) < 0 && errno == EINVAL, "writev() of a negative count: EINVAL");
    errno = 0;
    REQUIRE(writev(fd, out, IOV_MAX + 1) < 0 && errno == EINVAL, "writev() of more than IOV_MAX: EINVAL");
    errno = 0;
    REQUIRE(writev(9999, out, 2) < 0 && errno == EBADF, "writev() of nothing: EBADF");
    iovec empty[2] = { { one, 0 }, { two, 0 } };
    REQUIRE(writev(fd, empty, 2) == 0, "writev() of empty buffers: 0");
    iovec bad[1] = { { reinterpret_cast<void*>(1), 3 } };
    errno = 0;
    REQUIRE(writev(fd, bad, 1) < 0 && errno == EFAULT, "writev() of a bad pointer: EFAULT");
    close(fd);

    int fds[2];
    (void)pipe(fds);
    REQUIRE(writev(fds[1], out, 2) == 6, "writev() to a pipe");
    char received[8] = {};
    REQUIRE(read(fds[0], received, 8) == 6 && memcmp(received, "abcdef", 6) == 0, "a pipe gets it all, in order");
    close(fds[0]);
    close(fds[1]);

    fd = open(path("vectors"), O_RDONLY);
    char at[3] = {};
    REQUIRE(pread(fd, at, 2, 4) == 2 && memcmp(at, "ef", 2) == 0, "pread()");
    REQUIRE(lseek(fd, 0, SEEK_CUR) == 0, "pread() leaves the offset");
    errno = 0;
    REQUIRE(pread(fd, at, 2, -1) < 0 && errno == EINVAL, "pread() at a negative offset: EINVAL");
    close(fd);
    (void)pipe(fds);
    errno = 0;
    REQUIRE(pread(fds[0], at, 1, 0) < 0 && errno == ESPIPE, "pread() of a pipe: ESPIPE");
    errno = 0;
    REQUIRE(lseek(fds[0], 0, SEEK_SET) < 0 && errno == ESPIPE, "lseek() of a pipe: ESPIPE");
    close(fds[0]);
    close(fds[1]);
    end_area();
}

void set_address(sockaddr_un& address, char const* name)
{
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", name);
    address.sun_len = static_cast<unsigned char>(SUN_LEN(&address));
}

void unix_sockets()
{
    begin("unix sockets");
    char const* name = path("socket");
    unlink(name);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listener >= 0, "socket(AF_UNIX, SOCK_STREAM)");
    errno = 0;
    int bad_protocol = socket(AF_UNIX, SOCK_STREAM, 99);
    REQUIRE(bad_protocol < 0 && errno == EPROTONOSUPPORT, "socket(AF_UNIX) of another protocol: EPROTONOSUPPORT");
    errno = 0;
    int bad_family = socket(99, SOCK_STREAM, 0);
    REQUIRE(bad_family < 0 && errno == EAFNOSUPPORT, "socket() of no such family: EAFNOSUPPORT");
    errno = 0;
    REQUIRE(socket(AF_UNIX, 99, 0) < 0 && (errno == EPROTOTYPE || errno == EPROTONOSUPPORT), "socket() of no such type");

    sockaddr_un address;
    set_address(address, name);
    errno = 0;
    REQUIRE(listen(listener, 5) < 0 && errno == EINVAL, "listen() before bind(): EINVAL");
    REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) == 0, "bind()");
    struct stat status;
    REQUIRE(lstat(name, &status) == 0 && S_ISSOCK(status.st_mode), "bind() makes a socket's file");
    int second = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    REQUIRE(bind(second, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) < 0 && errno == EADDRINUSE, "bind() of a name in use: EADDRINUSE");
    close(second);
    errno = 0;
    int opened = open(name, O_RDONLY);
    REQUIRE(opened < 0 && errno == EOPNOTSUPP, "open() of a socket's file: EOPNOTSUPP");
    REQUIRE(listen(listener, 5) == 0, "listen()");

    sockaddr_un got {};
    socklen_t got_size = sizeof(got);
    REQUIRE(getsockname(listener, reinterpret_cast<sockaddr*>(&got), &got_size) == 0, "getsockname()");
    REQUIRE(got.sun_family == AF_UNIX && strcmp(got.sun_path, name) == 0, "getsockname() is its name");

    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    char byte = 'x';
    REQUIRE(write(client, &byte, 1) < 0 && (errno == ENOTCONN || errno == EPIPE || errno == EDESTADDRREQ), "write() before connecting");
    errno = 0;
    sockaddr_un peer {};
    socklen_t peer_size = sizeof(peer);
    REQUIRE(getpeername(client, reinterpret_cast<sockaddr*>(&peer), &peer_size) < 0 && errno == ENOTCONN, "getpeername() before connecting: ENOTCONN");
    REQUIRE(connect(client, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) == 0, "connect()");
    errno = 0;
    REQUIRE(connect(client, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) < 0 && errno == EISCONN, "connect() again: EISCONN");
    peer_size = sizeof(peer);
    REQUIRE(getpeername(client, reinterpret_cast<sockaddr*>(&peer), &peer_size) == 0 && strcmp(peer.sun_path, name) == 0, "getpeername() is who it connected to");

    pollfd waiting { listener, POLLIN, 0 };
    REQUIRE(poll(&waiting, 1, 2000) == 1 && (waiting.revents & POLLIN), "poll() says a connection's waiting");
    int server = accept(listener, nullptr, nullptr);
    REQUIRE(server >= 0, "accept()");
    REQUIRE(poll(&waiting, 1, 0) == 0, "and now one isn't");

    REQUIRE(write(client, "hello", 5) == 5, "write() to the other end");
    pollfd readable { server, POLLIN, 0 };
    REQUIRE(poll(&readable, 1, 2000) == 1 && (readable.revents & POLLIN), "poll() says there's something");
    int waiting_bytes = 0;
    REQUIRE(ioctl(server, FIONREAD, &waiting_bytes) == 0 && waiting_bytes == 5, "FIONREAD says how much");
    char buffer[16] = {};
    REQUIRE(recv(server, buffer, 2, MSG_PEEK) == 2 && memcmp(buffer, "he", 2) == 0, "recv(MSG_PEEK)");
    REQUIRE(read(server, buffer, sizeof(buffer)) == 5 && memcmp(buffer, "hello", 5) == 0, "read() gets it, peeking took nothing");
    REQUIRE(send(server, "back", 4, 0) == 4, "send() goes the other way");
    memset(buffer, 0, sizeof(buffer));
    REQUIRE(recv(client, buffer, sizeof(buffer), 0) == 4 && memcmp(buffer, "back", 4) == 0, "recv()");
    errno = 0;
    REQUIRE(recv(client, buffer, sizeof(buffer), MSG_DONTWAIT) < 0 && errno == EAGAIN, "recv(MSG_DONTWAIT) of nothing: EAGAIN");
    fcntl(client, F_SETFL, O_NONBLOCK);
    errno = 0;
    REQUIRE(read(client, buffer, sizeof(buffer)) < 0 && errno == EAGAIN, "read() on a nonblocking one: EAGAIN");
    fcntl(client, F_SETFL, 0);

    int type = 0;
    socklen_t type_size = sizeof(type);
    REQUIRE(getsockopt(client, SOL_SOCKET, SO_TYPE, &type, &type_size) == 0 && type == SOCK_STREAM, "getsockopt(SO_TYPE)");
    int error_value = 1;
    socklen_t error_size = sizeof(error_value);
    REQUIRE(getsockopt(client, SOL_SOCKET, SO_ERROR, &error_value, &error_size) == 0 && error_value == 0, "getsockopt(SO_ERROR)");
    int size_value = 0;
    socklen_t size_size = sizeof(size_value);
    REQUIRE(getsockopt(client, SOL_SOCKET, SO_SNDBUF, &size_value, &size_size) == 0 && size_value > 0, "getsockopt(SO_SNDBUF)");
    xucred credentials {};
    socklen_t credentials_size = sizeof(credentials);
    REQUIRE(getsockopt(server, SOL_LOCAL, LOCAL_PEERCRED, &credentials, &credentials_size) == 0, "LOCAL_PEERCRED");
    REQUIRE(credentials.cr_uid == geteuid(), "LOCAL_PEERCRED says who");
    uid_t peer_uid = 1;
    gid_t peer_gid = 1;
    REQUIRE(getpeereid(server, &peer_uid, &peer_gid) == 0 && peer_uid == geteuid(), "getpeereid()");
    struct stat of_socket;
    REQUIRE(fstat(client, &of_socket) == 0 && S_ISSOCK(of_socket.st_mode), "fstat() says it's a socket");
    errno = 0;
    REQUIRE(lseek(client, 0, SEEK_SET) < 0 && errno == ESPIPE, "lseek() of a socket: ESPIPE");

    REQUIRE(shutdown(client, SHUT_WR) == 0, "shutdown(SHUT_WR)");
    REQUIRE(read(server, buffer, sizeof(buffer)) == 0, "the other end reads the end of the file");
    REQUIRE(send(server, "z", 1, 0) == 1, "the other way still works");
    REQUIRE(read(client, buffer, 1) == 1 && buffer[0] == 'z', "and it arrives");
    errno = 0;
    REQUIRE(shutdown(client, 99) < 0 && errno == EINVAL, "shutdown() of no such way: EINVAL");

    int no_signal = 1;
    REQUIRE(setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal)) == 0, "setsockopt(SO_NOSIGPIPE)");
    close(client);
    (void)read(server, buffer, sizeof(buffer));
    errno = 0;
    REQUIRE(write(server, "x", 1) < 0 && errno == EPIPE, "write() to no one, with SO_NOSIGPIPE: EPIPE and no signal");
    close(server);
    close(listener);

    REQUIRE(stat(name, &status) == 0, "closing doesn't take the name away");
    int too_late = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    REQUIRE(connect(too_late, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) < 0 && errno == ECONNREFUSED, "connect() to a name nobody has: ECONNREFUSED");
    close(too_late);
    REQUIRE(unlink(name) == 0, "unlink() takes the name");
    too_late = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    REQUIRE(connect(too_late, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) < 0 && errno == ENOENT, "connect() to no name: ENOENT");
    write_file("not-a-socket", "x");
    sockaddr_un plain;
    set_address(plain, path("not-a-socket"));
    errno = 0;
    REQUIRE(connect(too_late, reinterpret_cast<sockaddr*>(&plain), SUN_LEN(&plain)) < 0 && errno == ENOTSOCK, "connect() to a file: ENOTSOCK");
    close(too_late);

    // A backlog that's full. A datagram's name, with the wrong type.
    listener = socket(AF_UNIX, SOCK_DGRAM, 0);
    REQUIRE(listener >= 0, "socket(AF_UNIX, SOCK_DGRAM)");
    REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) == 0, "bind() of a datagram socket");
    errno = 0;
    REQUIRE(listen(listener, 1) < 0 && errno == EOPNOTSUPP, "listen() on a datagram socket: EOPNOTSUPP");
    int wrong = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    REQUIRE(connect(wrong, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) < 0 && errno == EPROTOTYPE, "a stream can't connect to a datagram socket: EPROTOTYPE");
    close(wrong);

    int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    errno = 0;
    REQUIRE(send(sender, "q", 1, 0) < 0 && (errno == ENOTCONN || errno == EDESTADDRREQ), "send() on a datagram socket with nowhere to go");
    REQUIRE(sendto(sender, "first", 5, 0, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) == 5, "sendto()");
    REQUIRE(sendto(sender, "second!", 7, 0, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) == 7, "sendto() another");
    memset(buffer, 0, sizeof(buffer));
    REQUIRE(recv(listener, buffer, sizeof(buffer), 0) == 5 && memcmp(buffer, "first", 5) == 0, "each datagram's its own");
    REQUIRE(recv(listener, buffer, 3, 0) == 3 && memcmp(buffer, "sec", 3) == 0, "a small buffer gets as much as fits");
    errno = 0;
    REQUIRE(recv(listener, buffer, sizeof(buffer), MSG_DONTWAIT) < 0 && errno == EAGAIN, "and the rest of it is gone");
    REQUIRE(connect(sender, reinterpret_cast<sockaddr*>(&address), SUN_LEN(&address)) == 0, "connect() on a datagram socket");
    REQUIRE(send(sender, "third", 5, 0) == 5, "then send() knows where");
    sockaddr_un from {};
    socklen_t from_size = sizeof(from);
    REQUIRE(recvfrom(listener, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_size) == 5, "recvfrom()");
    close(sender);
    close(listener);
    unlink(name);

    errno = 0;
    int bad = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un wrong_family {};
    wrong_family.sun_family = AF_INET;
    strcpy(wrong_family.sun_path, "nope");
    REQUIRE(bind(bad, reinterpret_cast<sockaddr*>(&wrong_family), sizeof(wrong_family)) < 0 && errno == EAFNOSUPPORT, "bind() to another family's address: EAFNOSUPPORT");
    close(bad);
    errno = 0;
    REQUIRE(accept(0, nullptr, nullptr) < 0 && errno == ENOTSOCK, "accept() on something else: ENOTSOCK");
    errno = 0;
    REQUIRE(accept(9999, nullptr, nullptr) < 0 && errno == EBADF, "accept() of nothing: EBADF");
    end_area();
}

void socket_pairs()
{
    begin("socket pairs");
    int pair[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair(SOCK_STREAM)");
    REQUIRE(write(pair[0], "ab", 2) == 2, "one way");
    char buffer[8] = {};
    REQUIRE(read(pair[1], buffer, 8) == 2 && memcmp(buffer, "ab", 2) == 0, "arrives at the other");
    REQUIRE(write(pair[1], "cd", 2) == 2, "and the other way");
    REQUIRE(read(pair[0], buffer, 8) == 2 && memcmp(buffer, "cd", 2) == 0, "arrives at the first");
    pollfd can_write { pair[0], POLLOUT, 0 };
    REQUIRE(poll(&can_write, 1, 0) == 1 && (can_write.revents & POLLOUT), "poll() says there's room");

    // Fills, and then says so.
    fcntl(pair[0], F_SETFL, O_NONBLOCK);
    static char chunk[4096];
    size_t written = 0;
    for (;;) {
        ssize_t const put = write(pair[0], chunk, sizeof(chunk));
        if (put <= 0)
            break;
        written += static_cast<size_t>(put);
        if (written > 16 * 1024 * 1024)
            break;
    }
    REQUIRE(errno == EAGAIN && written > 0 && written <= 16 * 1024 * 1024, "a nonblocking write to a full one: EAGAIN");
    pollfd full { pair[0], POLLOUT, 0 };
    REQUIRE(poll(&full, 1, 0) == 0, "poll() says there isn't room");
    size_t drained = 0;
    fcntl(pair[1], F_SETFL, O_NONBLOCK);
    for (;;) {
        ssize_t const got = read(pair[1], chunk, sizeof(chunk));
        if (got <= 0)
            break;
        drained += static_cast<size_t>(got);
    }
    REQUIRE(drained == written, "all of what went in comes out");
    fcntl(pair[0], F_SETFL, 0);
    fcntl(pair[1], F_SETFL, 0);

    // A blocked writer carries on when there's room.
    pid_t child = fork();
    if (child == 0) {
        close(pair[1]);
        static char lots[256 * 1024];
        ssize_t const put = write(pair[0], lots, sizeof(lots));
        _exit(put == sizeof(lots) ? 0 : 1);
    }
    size_t total = 0;
    while (total < 256 * 1024) {
        ssize_t const got = read(pair[1], chunk, sizeof(chunk));
        if (got <= 0)
            break;
        total += static_cast<size_t>(got);
    }
    int exit_status = -1;
    waitpid(child, &exit_status, 0);
    REQUIRE(total == 256 * 1024 && WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0, "a large write gets through a socket, however long that takes");

    // SIGPIPE, and the end of the file.
    close(pair[0]);
    REQUIRE(read(pair[1], buffer, 8) == 0, "the other end closed: the end of the file");
    pollfd hung { pair[1], POLLIN, 0 };
    REQUIRE(poll(&hung, 1, 0) == 1 && (hung.revents & (POLLHUP | POLLIN)), "poll() of one with nobody there");
    auto previous = signal(SIGPIPE, SIG_IGN);
    errno = 0;
    REQUIRE(write(pair[1], "x", 1) < 0 && errno == EPIPE, "write() to nobody: EPIPE");
    signal(SIGPIPE, previous);
    child = fork();
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        (void)write(pair[1], "x", 1);
        _exit(0);
    }
    exit_status = 0;
    waitpid(child, &exit_status, 0);
    REQUIRE(WIFSIGNALED(exit_status) && WTERMSIG(exit_status) == SIGPIPE, "write() to nobody: SIGPIPE");
    close(pair[1]);

    int datagrams[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM, 0, datagrams) == 0, "socketpair(SOCK_DGRAM)");
    REQUIRE(send(datagrams[0], "one", 3, 0) == 3 && send(datagrams[0], "three", 5, 0) == 5, "datagrams keep their shape");
    REQUIRE(recv(datagrams[1], buffer, 8, 0) == 3, "one");
    REQUIRE(recv(datagrams[1], buffer, 8, 0) == 5, "and another");
    close(datagrams[0]);
    close(datagrams[1]);
    errno = 0;
    REQUIRE(socketpair(AF_INET, SOCK_STREAM, 0, pair) < 0 && (errno == EOPNOTSUPP || errno == EAFNOSUPPORT), "socketpair() of an internet socket");

    // sendmsg, recvmsg.
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "another pair");
    iovec out[2] = { { const_cast<char*>("mes"), 3 }, { const_cast<char*>("sage"), 4 } };
    msghdr message {};
    message.msg_iov = out;
    message.msg_iovlen = 2;
    REQUIRE(sendmsg(pair[0], &message, 0) == 7, "sendmsg() of two buffers");
    char first[4] = {}, second[8] = {};
    iovec in[2] = { { first, 4 }, { second, 8 } };
    msghdr received {};
    received.msg_iov = in;
    received.msg_iovlen = 2;
    REQUIRE(recvmsg(pair[1], &received, 0) == 7, "recvmsg() into two");
    REQUIRE(memcmp(first, "mess", 4) == 0 && memcmp(second, "age", 3) == 0, "each gets what fits");
    REQUIRE(received.msg_flags == 0, "recvmsg() with nothing to say about it");
    close(pair[0]);
    close(pair[1]);
    end_area();
}

void passing_files()
{
    begin("passing files");
    int pair[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "a pair to send through");
    write_file("passed", "the contents");
    int file = open(path("passed"), O_RDONLY);
    REQUIRE(lseek(file, 4, SEEK_SET) == 4, "a file part way through");

    auto send_with = [&](int sent, char const* text) {
        iovec one { const_cast<char*>(text), strlen(text) };
        char space[CMSG_SPACE(sizeof(int))] = {};
        msghdr message {};
        message.msg_iov = &one;
        message.msg_iovlen = 1;
        message.msg_control = space;
        message.msg_controllen = sizeof(space);
        auto* control = CMSG_FIRSTHDR(&message);
        control->cmsg_level = SOL_SOCKET;
        control->cmsg_type = SCM_RIGHTS;
        control->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(control), &sent, sizeof(int));
        return sendmsg(pair[0], &message, 0);
    };
    REQUIRE(send_with(file, "with") == 4, "sendmsg() with a descriptor");

    pid_t child = fork();
    if (child == 0) {
        char text[8] = {};
        iovec in { text, sizeof(text) };
        char space[CMSG_SPACE(sizeof(int))] = {};
        msghdr message {};
        message.msg_iov = &in;
        message.msg_iovlen = 1;
        message.msg_control = space;
        message.msg_controllen = sizeof(space);
        close(file);
        if (recvmsg(pair[1], &message, 0) != 4)
            _exit(1);
        auto* control = CMSG_FIRSTHDR(&message);
        if (!control || control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS)
            _exit(2);
        if (control->cmsg_len != CMSG_LEN(sizeof(int)))
            _exit(3);
        int got = -1;
        memcpy(&got, CMSG_DATA(control), sizeof(int));
        char contents[16] = {};
        if (read(got, contents, 8) != 8)
            _exit(4);
        // It's the same open file: where it was, and now further on.
        if (memcmp(contents, "contents", 8) != 0)
            _exit(5);
        _exit(0);
    }
    int status = -1;
    waitpid(child, &status, 0);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a descriptor arrives in another process, and it's the same open file");
    REQUIRE(lseek(file, 0, SEEK_CUR) == 12, "they share where they've got to");

    REQUIRE(send_with(file, "with") == 4, "another");
    char text[8] = {};
    iovec in { text, sizeof(text) };
    char small[1] = {};
    msghdr message {};
    message.msg_iov = &in;
    message.msg_iovlen = 1;
    message.msg_control = small;
    message.msg_controllen = sizeof(small);
    REQUIRE(recvmsg(pair[1], &message, 0) == 4, "recvmsg() with no room for the descriptor");
    REQUIRE(message.msg_flags & MSG_CTRUNC, "says it's been cut: MSG_CTRUNC");

    int sent = pipe(pair) == 0 ? 0 : -1;
    REQUIRE(sent == 0, "a pipe too");
    close(pair[0]);
    close(pair[1]);
    close(file);

    int other[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, other) == 0, "a pair for a pipe");
    int through[2];
    (void)pipe(through);
    iovec byte { const_cast<char*>("p"), 1 };
    char space[CMSG_SPACE(sizeof(int))] = {};
    msghdr with_pipe {};
    with_pipe.msg_iov = &byte;
    with_pipe.msg_iovlen = 1;
    with_pipe.msg_control = space;
    with_pipe.msg_controllen = sizeof(space);
    auto* control = CMSG_FIRSTHDR(&with_pipe);
    control->cmsg_level = SOL_SOCKET;
    control->cmsg_type = SCM_RIGHTS;
    control->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(control), &through[1], sizeof(int));
    REQUIRE(sendmsg(other[0], &with_pipe, 0) == 1, "sendmsg() of a pipe's end");
    close(through[1]);
    char landing[1];
    iovec into { landing, 1 };
    char received_space[CMSG_SPACE(sizeof(int))] = {};
    msghdr receive {};
    receive.msg_iov = &into;
    receive.msg_iovlen = 1;
    receive.msg_control = received_space;
    receive.msg_controllen = sizeof(received_space);
    REQUIRE(recvmsg(other[1], &receive, 0) == 1, "recvmsg() of it");
    int writer = -1;
    memcpy(&writer, CMSG_DATA(CMSG_FIRSTHDR(&receive)), sizeof(int));
    REQUIRE(write(writer, "!", 1) == 1, "writing to it");
    REQUIRE(read(through[0], landing, 1) == 1 && landing[0] == '!', "it comes out of the pipe");
    close(writer);
    REQUIRE(read(through[0], landing, 1) == 0, "and when it closes, the pipe ends");
    close(through[0]);

    // Sent and never received: closes when the socket does.
    int never[2];
    (void)pipe(never);
    memcpy(CMSG_DATA(control), &never[1], sizeof(int));
    REQUIRE(sendmsg(other[0], &with_pipe, 0) == 1, "sendmsg() of what nobody takes");
    close(never[1]);
    close(other[0]);
    close(other[1]);
    REQUIRE(read(never[0], landing, 1) == 0, "it closes when the socket does");
    close(never[0]);

    errno = 0;
    (void)socketpair(AF_UNIX, SOCK_STREAM, 0, other);
    int const nothing = 9999;
    control = CMSG_FIRSTHDR(&with_pipe);
    memcpy(CMSG_DATA(control), &nothing, sizeof(int));
    REQUIRE(sendmsg(other[0], &with_pipe, 0) < 0 && errno == EBADF, "sendmsg() of no descriptor: EBADF");
    close(other[0]);
    close(other[1]);
    end_area();
}

void kqueues()
{
    begin("kqueue");
    int queue = kqueue();
    REQUIRE(queue >= 0, "kqueue()");
    struct kevent change;
    struct kevent event;
    timespec none { 0, 0 };
    timespec brief { 0, 50 * 1000 * 1000 };
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "nothing to say yet");

    int fds[2];
    (void)pipe(fds);
    EV_SET(&change, fds[0], EVFILT_READ, EV_ADD, 0, 0, reinterpret_cast<void*>(0x1234));
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0, "EVFILT_READ on a pipe");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "nothing's there");
    (void)write(fds[1], "abc", 3);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &brief) == 1, "something's there");
    REQUIRE(event.filter == EVFILT_READ && event.ident == static_cast<uintptr_t>(fds[0]), "says which");
    REQUIRE(event.data == 3, "says how much");
    REQUIRE(event.udata == reinterpret_cast<void*>(0x1234), "gives back what it was given");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 1, "says so until it's read");
    char buffer[8];
    (void)read(fds[0], buffer, 3);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "and then doesn't");

    EV_SET(&change, fds[0], EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0, "EV_DELETE");
    (void)write(fds[1], "x", 1);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "deleted, it says nothing");
    errno = 0;
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) < 0 && errno == ENOENT, "EV_DELETE of nothing: ENOENT");

    EV_SET(&change, fds[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 1, "EV_ONESHOT says so once");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "and that's all");

    EV_SET(&change, fds[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 1, "EV_CLEAR says so");
    EV_SET(&change, fds[0], EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    (void)read(fds[0], buffer, 1);

    EV_SET(&change, fds[0], EVFILT_READ, EV_ADD | EV_DISABLE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    (void)write(fds[1], "y", 1);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "EV_DISABLE keeps quiet");
    EV_SET(&change, fds[0], EVFILT_READ, EV_ENABLE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 1, "EV_ENABLE says what's there");
    EV_SET(&change, fds[0], EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);

    EV_SET(&change, fds[1], EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 1 && event.filter == EVFILT_WRITE && event.data > 0, "EVFILT_WRITE says how much room");
    EV_SET(&change, fds[1], EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);

    EV_SET(&change, fds[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    (void)read(fds[0], buffer, 8);
    close(fds[1]);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &brief) == 1 && (event.flags & EV_EOF), "EV_EOF when the other end closes");
    close(fds[0]);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "closing a descriptor takes what's watching it");

    EV_SET(&change, 9999, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    errno = 0;
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) < 0 && errno == EBADF, "EVFILT_READ of nothing: EBADF");
    REQUIRE(kevent(queue, &change, 1, &event, 1, &none) == 1 && (event.flags & EV_ERROR) && event.data == EBADF, "with room to say so: EV_ERROR");
    EV_SET(&change, 1, static_cast<short>(-99), EV_ADD, 0, 0, nullptr);
    errno = 0;
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) < 0 && errno == EINVAL, "no such filter: EINVAL");
    errno = 0;
    REQUIRE(kevent(9999, nullptr, 0, nullptr, 0, nullptr) < 0 && errno == EBADF, "kevent() of nothing: EBADF");
    errno = 0;
    REQUIRE(kevent(0, nullptr, 0, nullptr, 0, nullptr) < 0 && (errno == EBADF || errno == EINVAL), "kevent() of something else");

    EV_SET(&change, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS, 2000, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    timespec patient { 2, 0 };
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &patient) == 1 && event.filter == EVFILT_TIMER, "EVFILT_TIMER, once");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "just the once");
    EV_SET(&change, 2, EVFILT_TIMER, EV_ADD, 0, 5, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &patient) == 1 && event.ident == 2, "EVFILT_TIMER, in milliseconds");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &patient) == 1 && event.ident == 2, "and again");
    EV_SET(&change, 2, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);

    EV_SET(&change, 3, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0, "EVFILT_USER");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "until it's triggered, nothing");
    EV_SET(&change, 3, EVFILT_USER, 0, NOTE_TRIGGER | NOTE_FFCOPY | 0x55, 0, nullptr);
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0, "NOTE_TRIGGER");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 1 && event.ident == 3 && event.fflags == 0x55, "then it says so, with its flags");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "and EV_CLEAR clears it");
    EV_SET(&change, 3, EVFILT_USER, EV_DELETE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);

    auto previous = signal(SIGUSR1, SIG_IGN);
    EV_SET(&change, SIGUSR1, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0, "EVFILT_SIGNAL");
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR1);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &patient) == 1 && event.ident == SIGUSR1 && event.data == 2, "says how many, ignored or not");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "and starts counting again");
    EV_SET(&change, SIGUSR1, EVFILT_SIGNAL, EV_DELETE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    signal(SIGUSR1, previous);

    int sync_pipe[2];
    (void)pipe(sync_pipe);
    pid_t child = fork();
    if (child == 0) {
        char wait_byte;
        (void)read(sync_pipe[0], &wait_byte, 1);
        _exit(7);
    }
    EV_SET(&change, child, EVFILT_PROC, EV_ADD, NOTE_EXIT | NOTE_EXITSTATUS, 0, nullptr);
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0, "EVFILT_PROC");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "while it's alive, nothing");
    (void)write(sync_pipe[1], "g", 1);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &patient) == 1 && (event.fflags & NOTE_EXIT), "NOTE_EXIT when it ends");
    REQUIRE(WIFEXITED(event.data) && WEXITSTATUS(event.data) == 7, "NOTE_EXITSTATUS says how");
    REQUIRE(event.flags & EV_EOF, "and it's over");
    waitpid(child, nullptr, 0);
    close(sync_pipe[0]);
    close(sync_pipe[1]);
    EV_SET(&change, 999999, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    errno = 0;
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) < 0 && errno == ESRCH, "EVFILT_PROC of nobody: ESRCH");

    write_file("watched", "start");
    int watched = open(path("watched"), O_RDONLY);
    EV_SET(&change, watched, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE, 0, nullptr);
    REQUIRE(kevent(queue, &change, 1, nullptr, 0, nullptr) == 0, "EVFILT_VNODE");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &none) == 0, "until it changes, nothing");
    int writing = open(path("watched"), O_WRONLY | O_APPEND);
    (void)write(writing, "more", 4);
    close(writing);
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &patient) == 1 && (event.fflags & NOTE_WRITE) && (event.fflags & NOTE_EXTEND), "NOTE_WRITE and NOTE_EXTEND");
    REQUIRE(unlink(path("watched")) == 0, "unlink() of what's watched");
    REQUIRE(kevent(queue, nullptr, 0, &event, 1, &patient) == 1 && (event.fflags & NOTE_DELETE), "NOTE_DELETE");
    close(watched);

    pollfd on_queue { queue, POLLIN, 0 };
    REQUIRE(poll(&on_queue, 1, 0) == 0, "poll() of a quiet kqueue");
    EV_SET(&change, 4, EVFILT_USER, EV_ADD, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    EV_SET(&change, 4, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
    REQUIRE(poll(&on_queue, 1, 0) == 1 && (on_queue.revents & POLLIN), "poll() of a kqueue with something to say");
    int inherited[2];
    (void)pipe(inherited);
    child = fork();
    if (child == 0) {
        struct stat status;
        char const result = fstat(queue, &status) < 0 && errno == EBADF ? 'y' : 'n';
        (void)write(inherited[1], &result, 1);
        _exit(0);
    }
    char result = 0;
    (void)read(inherited[0], &result, 1);
    waitpid(child, nullptr, 0);
    close(inherited[0]);
    close(inherited[1]);
    REQUIRE(result == 'y', "fork() doesn't pass on a kqueue");
    struct stat of_queue;
    REQUIRE(fstat(queue, &of_queue) == 0, "fstat() of a kqueue");
    errno = 0;
    REQUIRE(read(queue, buffer, 1) < 0, "read() of a kqueue");
    close(queue);
    end_area();
}

volatile sig_atomic_t s_interrupted = 0;
void count_interrupt(int) { s_interrupted = s_interrupted + 1; }

void selects()
{
    begin("select");
    int fds[2];
    (void)pipe(fds);
    fd_set reading;
    FD_ZERO(&reading);
    FD_SET(fds[0], &reading);
    REQUIRE(FD_ISSET(fds[0], &reading), "FD_SET and FD_ISSET");
    FD_CLR(fds[0], &reading);
    REQUIRE(!FD_ISSET(fds[0], &reading), "FD_CLR");
    FD_SET(fds[0], &reading);
    timeval none { 0, 0 };
    REQUIRE(select(fds[0] + 1, &reading, nullptr, nullptr, &none) == 0, "select() with nothing to read");
    REQUIRE(!FD_ISSET(fds[0], &reading), "and it clears what isn't ready");
    (void)write(fds[1], "x", 1);
    FD_SET(fds[0], &reading);
    REQUIRE(select(fds[0] + 1, &reading, nullptr, nullptr, &none) == 1 && FD_ISSET(fds[0], &reading), "select() of something to read");
    fd_set writing;
    FD_ZERO(&writing);
    FD_SET(fds[1], &writing);
    REQUIRE(select(fds[1] + 1, nullptr, &writing, nullptr, &none) == 1 && FD_ISSET(fds[1], &writing), "select() of room to write");
    timeval brief { 0, 20 * 1000 };
    timespec started, ended;
    clock_gettime(CLOCK_MONOTONIC, &started);
    FD_ZERO(&reading);
    REQUIRE(select(0, &reading, nullptr, nullptr, &brief) == 0, "select() of nothing just waits");
    clock_gettime(CLOCK_MONOTONIC, &ended);
    long const waited = (ended.tv_sec - started.tv_sec) * 1000 + (ended.tv_nsec - started.tv_nsec) / 1000000;
    REQUIRE(waited >= 15, "for about as long as it was told");
    FD_ZERO(&reading);
    FD_SET(50, &reading);
    errno = 0;
    REQUIRE(select(51, &reading, nullptr, nullptr, &none) < 0 && errno == EBADF, "select() of what isn't open: EBADF");
    errno = 0;
    REQUIRE(select(-1, nullptr, nullptr, nullptr, &none) < 0 && errno == EINVAL, "select() of a negative count: EINVAL");
    REQUIRE(FD_SETSIZE >= 1024, "FD_SETSIZE");
    timespec a_while { 0, 1000 };
    sigset_t mask;
    sigemptyset(&mask);
    FD_ZERO(&reading);
    FD_SET(fds[0], &reading);
    REQUIRE(pselect(fds[0] + 1, &reading, nullptr, nullptr, &a_while, &mask) == 1, "pselect()");

    struct sigaction handler {};
    handler.sa_handler = count_interrupt;
    sigaction(SIGALRM, &handler, nullptr);
    char byte;
    (void)read(fds[0], &byte, 1);
    FD_ZERO(&reading);
    FD_SET(fds[0], &reading);
    itimerval soon {};
    soon.it_value.tv_usec = 5000;
    setitimer(ITIMER_REAL, &soon, nullptr);
    errno = 0;
    REQUIRE(select(fds[0] + 1, &reading, nullptr, nullptr, nullptr) < 0 && errno == EINTR, "a signal ends select(): EINTR");
    signal(SIGALRM, SIG_DFL);
    close(fds[0]);
    close(fds[1]);
    end_area();
}

void fifos()
{
    begin("fifos");
    char const* name = path("fifo");
    REQUIRE(mkfifo(name, 0644) == 0, "mkfifo()");
    struct stat status;
    REQUIRE(stat(name, &status) == 0 && S_ISFIFO(status.st_mode), "stat() says it's a pipe");
    REQUIRE((status.st_mode & 0777) == 0644, "with the mode it was given");
    errno = 0;
    REQUIRE(mkfifo(name, 0644) < 0 && errno == EEXIST, "mkfifo() of what's there: EEXIST");

    errno = 0;
    REQUIRE(open(name, O_WRONLY | O_NONBLOCK) < 0 && errno == ENXIO, "open() to write, with nobody reading, not waiting: ENXIO");
    int reader = open(name, O_RDONLY | O_NONBLOCK);
    REQUIRE(reader >= 0, "open() to read, not waiting, works");
    char buffer[16] = {};
    REQUIRE(read(reader, buffer, sizeof(buffer)) == 0, "with no writer, the end of the file");
    int writer = open(name, O_WRONLY | O_NONBLOCK);
    REQUIRE(writer >= 0, "open() to write, now there's a reader");
    errno = 0;
    REQUIRE(read(reader, buffer, sizeof(buffer)) < 0 && errno == EAGAIN, "with a writer and nothing written: EAGAIN");
    REQUIRE(write(writer, "piped", 5) == 5, "write()");
    REQUIRE(read(reader, buffer, sizeof(buffer)) == 5 && memcmp(buffer, "piped", 5) == 0, "read() gets it");
    REQUIRE(fstat(writer, &status) == 0 && S_ISFIFO(status.st_mode), "fstat() of one open");
    errno = 0;
    REQUIRE(lseek(writer, 0, SEEK_SET) < 0 && errno == ESPIPE, "lseek() of one: ESPIPE");
    close(writer);
    REQUIRE(read(reader, buffer, sizeof(buffer)) == 0, "with the writer gone, the end of the file");
    close(reader);

    // Each end waits for the other.
    pid_t child = fork();
    if (child == 0) {
        int fd = open(name, O_WRONLY);
        if (fd < 0)
            _exit(1);
        (void)write(fd, "from child", 10);
        close(fd);
        _exit(0);
    }
    reader = open(name, O_RDONLY);
    REQUIRE(reader >= 0, "open() that waits for the other end");
    memset(buffer, 0, sizeof(buffer));
    size_t total = 0;
    ssize_t got;
    while (total < 10 && (got = read(reader, buffer + total, sizeof(buffer) - total)) > 0)
        total += static_cast<size_t>(got);
    REQUIRE(total == 10 && memcmp(buffer, "from child", 10) == 0, "another process's words come through");
    close(reader);
    int exit_status = -1;
    waitpid(child, &exit_status, 0);
    REQUIRE(WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0, "and it carried on too");

    int both = open(name, O_RDWR);
    REQUIRE(both >= 0, "open() for both doesn't wait");
    REQUIRE(write(both, "self", 4) == 4 && read(both, buffer, 4) == 4 && memcmp(buffer, "self", 4) == 0, "and it can talk to itself");
    close(both);

    // What's in it doesn't outlive everyone having it open.
    reader = open(name, O_RDONLY | O_NONBLOCK);
    writer = open(name, O_WRONLY | O_NONBLOCK);
    (void)write(writer, "lost", 4);
    close(writer);
    close(reader);
    reader = open(name, O_RDONLY | O_NONBLOCK);
    REQUIRE(read(reader, buffer, 4) == 0, "what's left in it when nobody has it open goes");
    close(reader);

    // A signal gets it out of waiting.
    struct sigaction handler {};
    handler.sa_handler = count_interrupt;
    sigaction(SIGALRM, &handler, nullptr);
    itimerval soon {};
    soon.it_value.tv_usec = 20000;
    setitimer(ITIMER_REAL, &soon, nullptr);
    errno = 0;
    int const interrupted = open(name, O_RDONLY);
    REQUIRE(interrupted < 0 && errno == EINTR, "a signal ends the wait: EINTR");
    signal(SIGALRM, SIG_DFL);

    REQUIRE(unlink(name) == 0, "unlink() of one");
    end_area();
}

void shared_memory()
{
    begin("shared memory");
    auto* shared = static_cast<int*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    REQUIRE(shared != MAP_FAILED, "mmap(MAP_SHARED | MAP_ANON)");
    REQUIRE(*shared == 0, "starts as zeroes");
    pid_t child = fork();
    if (child == 0) {
        *shared = 42;
        _exit(0);
    }
    waitpid(child, nullptr, 0);
    REQUIRE(*shared == 42, "a child's writes show in its parent");
    *shared = 7;
    child = fork();
    if (child == 0)
        _exit(*shared == 7 ? 0 : 1);
    int status = -1;
    waitpid(child, &status, 0);
    REQUIRE(WEXITSTATUS(status) == 0, "and a parent's in its child");
    REQUIRE(munmap(shared, 4096) == 0, "munmap() of it");

    auto* private_memory = static_cast<int*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    *private_memory = 1;
    child = fork();
    if (child == 0) {
        *private_memory = 2;
        _exit(0);
    }
    waitpid(child, nullptr, 0);
    REQUIRE(*private_memory == 1, "private memory stays private");

    REQUIRE(madvise(private_memory, 4096, MADV_NORMAL) == 0, "madvise(MADV_NORMAL)");
    REQUIRE(madvise(private_memory, 4096, MADV_WILLNEED) == 0, "madvise(MADV_WILLNEED)");
    REQUIRE(madvise(private_memory, 4096, MADV_SEQUENTIAL) == 0, "madvise(MADV_SEQUENTIAL)");
    REQUIRE(madvise(private_memory, 4096, MADV_RANDOM) == 0, "madvise(MADV_RANDOM)");
    REQUIRE(madvise(private_memory, 4096, MADV_FREE) == 0, "madvise(MADV_FREE)");
    REQUIRE(posix_madvise(private_memory, 4096, POSIX_MADV_NORMAL) == 0, "posix_madvise()");
    errno = 0;
    REQUIRE(madvise(private_memory, 4096, 9999) < 0 && errno == EINVAL, "madvise() of no such advice: EINVAL");
    REQUIRE(madvise(reinterpret_cast<char*>(private_memory) + 1, 4095, MADV_NORMAL) == 0, "madvise() of an address that isn't a page's");
    REQUIRE(munmap(private_memory, 4096) == 0, "munmap()");
    errno = 0;
    REQUIRE(madvise(private_memory, 4096, MADV_NORMAL) < 0 && (errno == ENOMEM || errno == EINVAL), "madvise() of what's not there");

    char name[64];
    snprintf(name, sizeof(name), "/vulture-posix-%d", getpid());
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    REQUIRE(fd >= 0, "shm_open(O_CREAT)");
    REQUIRE(ftruncate(fd, 4096) == 0, "ftruncate() of it");
    errno = 0;
    REQUIRE(shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600) < 0 && errno == EEXIST, "shm_open(O_EXCL) of what's there: EEXIST");
    auto* mapped = static_cast<char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    REQUIRE(mapped != MAP_FAILED, "mmap() of it");
    strcpy(mapped, "named");
    int second = shm_open(name, O_RDWR, 0);
    REQUIRE(second >= 0, "shm_open() of it by name");
    auto* again = static_cast<char*>(mmap(nullptr, 4096, PROT_READ, MAP_SHARED, second, 0));
    REQUIRE(again != MAP_FAILED && strcmp(again, "named") == 0, "the same memory");
    REQUIRE((fcntl(fd, F_GETFD) & FD_CLOEXEC), "close-on-exec, as it should be");
    munmap(again, 4096);
    munmap(mapped, 4096);
    close(second);
    close(fd);
    REQUIRE(shm_unlink(name) == 0, "shm_unlink()");
    errno = 0;
    REQUIRE(shm_open(name, O_RDWR, 0) < 0 && errno == ENOENT, "shm_open() of what's gone: ENOENT");
    errno = 0;
    REQUIRE(shm_unlink(name) < 0 && errno == ENOENT, "shm_unlink() of what's gone: ENOENT");
    end_area();
}

volatile sig_atomic_t s_alarms = 0;
void count_alarm(int) { s_alarms = s_alarms + 1; }

void interval_timers()
{
    begin("interval timers");
    itimerval value {};
    REQUIRE(getitimer(ITIMER_REAL, &value) == 0, "getitimer(ITIMER_REAL)");
    REQUIRE(value.it_value.tv_sec == 0 && value.it_value.tv_usec == 0, "nothing's set");
    errno = 0;
    REQUIRE(getitimer(99, &value) < 0 && errno == EINVAL, "getitimer() of no such timer: EINVAL");
    errno = 0;
    REQUIRE(setitimer(99, &value, nullptr) < 0 && errno == EINVAL, "setitimer() of no such timer: EINVAL");
    itimerval invalid {};
    invalid.it_value.tv_usec = 2000000;
    errno = 0;
    REQUIRE(setitimer(ITIMER_REAL, &invalid, nullptr) < 0 && errno == EINVAL, "setitimer() of too many microseconds: EINVAL");

    struct sigaction handler {};
    handler.sa_handler = count_alarm;
    sigaction(SIGALRM, &handler, nullptr);
    s_alarms = 0;
    itimerval once {};
    once.it_value.tv_usec = 2000;
    REQUIRE(setitimer(ITIMER_REAL, &once, nullptr) == 0, "setitimer(ITIMER_REAL)");
    timespec patience { 2, 0 };
    (void)nanosleep(&patience, nullptr);
    REQUIRE(s_alarms == 1, "SIGALRM, once");
    (void)getitimer(ITIMER_REAL, &value);
    REQUIRE(value.it_value.tv_sec == 0 && value.it_value.tv_usec == 0, "it's finished");

    itimerval repeating {};
    repeating.it_value.tv_usec = 2000;
    repeating.it_interval.tv_usec = 2000;
    s_alarms = 0;
    REQUIRE(setitimer(ITIMER_REAL, &repeating, nullptr) == 0, "setitimer() over and over");
    (void)getitimer(ITIMER_REAL, &value);
    REQUIRE(value.it_interval.tv_usec == 2000, "getitimer() has the interval");
    for (int i = 0; i < 5; ++i) {
        timespec a_while { 1, 0 };
        (void)nanosleep(&a_while, nullptr);
    }
    REQUIRE(s_alarms >= 5, "SIGALRM, again and again");
    itimerval off {};
    itimerval was {};
    REQUIRE(setitimer(ITIMER_REAL, &off, &was) == 0, "setitimer() of nothing stops it");
    REQUIRE(was.it_interval.tv_usec == 2000, "and says what it was");
    int const at_stop = s_alarms;
    timespec brief { 0, 10 * 1000 * 1000 };
    (void)nanosleep(&brief, nullptr);
    REQUIRE(s_alarms <= at_stop + 1, "and it's stopped");

    itimerval long_one {};
    long_one.it_value.tv_sec = 100;
    (void)setitimer(ITIMER_REAL, &long_one, nullptr);
    REQUIRE(alarm(0) >= 99, "alarm() says what's left of a timer");
    (void)getitimer(ITIMER_REAL, &value);
    REQUIRE(value.it_value.tv_sec == 0 && value.it_value.tv_usec == 0, "alarm(0) stops a timer");
    (void)alarm(100);
    (void)getitimer(ITIMER_REAL, &value);
    REQUIRE(value.it_value.tv_sec >= 99, "getitimer() sees an alarm");
    (void)alarm(0);

    pid_t child = fork();
    if (child == 0) {
        itimerval seen {};
        getitimer(ITIMER_REAL, &seen);
        _exit(seen.it_value.tv_sec == 0 && seen.it_value.tv_usec == 0 ? 0 : 1);
    }
    int status = -1;
    (void)setitimer(ITIMER_REAL, &long_one, nullptr);
    child = fork();
    if (child == 0) {
        itimerval seen {};
        getitimer(ITIMER_REAL, &seen);
        _exit(seen.it_value.tv_sec == 0 && seen.it_value.tv_usec == 0 ? 0 : 1);
    }
    waitpid(child, &status, 0);
    while (wait(nullptr) > 0) { }
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a child doesn't get the timer");
    (void)setitimer(ITIMER_REAL, &off, nullptr);
    signal(SIGALRM, SIG_DFL);

    // By the processor's time, not the clock's: burning it, not sleeping.
    static volatile sig_atomic_t s_virtual = 0;
    static volatile sig_atomic_t s_profile = 0;
    struct sigaction count_virtual {};
    count_virtual.sa_handler = [](int) { s_virtual = 1; };
    sigaction(SIGVTALRM, &count_virtual, nullptr);
    struct sigaction count_profile {};
    count_profile.sa_handler = [](int) { s_profile = 1; };
    sigaction(SIGPROF, &count_profile, nullptr);
    itimerval burning {};
    burning.it_value.tv_usec = 5000;
    REQUIRE(setitimer(ITIMER_VIRTUAL, &burning, nullptr) == 0, "setitimer(ITIMER_VIRTUAL)");
    timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    do {
        for (volatile int i = 0; i < 10000; i = i + 1) { }
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (!s_virtual && now.tv_sec - started.tv_sec < 5);
    REQUIRE(s_virtual, "SIGVTALRM, for processor time");
    REQUIRE(setitimer(ITIMER_PROF, &burning, nullptr) == 0, "setitimer(ITIMER_PROF)");
    clock_gettime(CLOCK_MONOTONIC, &started);
    do {
        for (volatile int i = 0; i < 10000; i = i + 1) { }
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (!s_profile && now.tv_sec - started.tv_sec < 5);
    REQUIRE(s_profile, "SIGPROF, for processor time");

    s_virtual = 0;
    (void)setitimer(ITIMER_VIRTUAL, &burning, nullptr);
    timespec sleeping { 0, 100 * 1000 * 1000 };
    (void)nanosleep(&sleeping, nullptr);
    REQUIRE(!s_virtual, "sleeping uses no processor time");
    (void)setitimer(ITIMER_VIRTUAL, &off, nullptr);
    signal(SIGVTALRM, SIG_DFL);
    signal(SIGPROF, SIG_DFL);
    end_area();
}

volatile sig_atomic_t s_restart_handled = 0;
void note_handled(int) { s_restart_handled = s_restart_handled + 1; }

// A process that writes one byte to `fd` after a little while.
pid_t write_later(int fd, long milliseconds)
{
    pid_t const child = fork();
    if (child == 0) {
        timespec a_while { milliseconds / 1000, (milliseconds % 1000) * 1000000 };
        (void)nanosleep(&a_while, nullptr);
        (void)write(fd, "!", 1);
        _exit(0);
    }
    return child;
}

void soon_alarm()
{
    itimerval soon {};
    soon.it_value.tv_usec = 5000;
    setitimer(ITIMER_REAL, &soon, nullptr);
}

void restarting()
{
    begin("restarting");
    int fds[2];
    (void)pipe(fds);
    char byte = 0;
    struct sigaction action {};
    action.sa_handler = note_handled;

    action.sa_flags = 0;
    sigaction(SIGALRM, &action, nullptr);
    s_restart_handled = 0;
    soon_alarm();
    errno = 0;
    REQUIRE(read(fds[0], &byte, 1) < 0 && errno == EINTR, "read(), with a handler: EINTR");
    REQUIRE(s_restart_handled == 1, "and the handler ran");

    action.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &action, nullptr);
    s_restart_handled = 0;
    pid_t writer = write_later(fds[1], 60);
    soon_alarm();
    errno = 0;
    REQUIRE(read(fds[0], &byte, 1) == 1 && byte == '!', "read(), with SA_RESTART: it carries on until there's something");
    REQUIRE(s_restart_handled == 1, "and the handler ran first");
    waitpid(writer, nullptr, 0);

    signal(SIGALRM, note_handled);
    s_restart_handled = 0;
    writer = write_later(fds[1], 60);
    soon_alarm();
    REQUIRE(read(fds[0], &byte, 1) == 1, "read(), with signal(): the same");
    REQUIRE(s_restart_handled == 1, "signal() means SA_RESTART");
    waitpid(writer, nullptr, 0);

    // Stopping isn't something a program would need to hear about.
    int report[2];
    (void)pipe(report);
    pid_t reader = fork();
    if (reader == 0) {
        char got = 0;
        errno = 0;
        ssize_t const result = read(fds[0], &got, 1);
        int const failure = result < 0 ? errno : 0;
        (void)write(report[1], &failure, sizeof(failure));
        _exit(0);
    }
    timespec settle { 0, 30 * 1000 * 1000 };
    (void)nanosleep(&settle, nullptr);
    kill(reader, SIGSTOP);
    int status = 0;
    (void)waitpid(reader, &status, WUNTRACED);
    REQUIRE(WIFSTOPPED(status), "SIGSTOP stops one blocked in read()");
    kill(reader, SIGCONT);
    (void)nanosleep(&settle, nullptr);
    (void)write(fds[1], "x", 1);
    int failure = -1;
    (void)read(report[0], &failure, sizeof(failure));
    REQUIRE(failure == 0, "SIGSTOP and SIGCONT: read() just carries on");
    waitpid(reader, nullptr, 0);

    // A signal that's ignored isn't anything.
    action.sa_handler = SIG_IGN;
    action.sa_flags = 0;
    sigaction(SIGUSR1, &action, nullptr);
    reader = fork();
    if (reader == 0) {
        char got = 0;
        ssize_t const result = read(fds[0], &got, 1);
        _exit(result == 1 ? 0 : 1);
    }
    (void)nanosleep(&settle, nullptr);
    kill(reader, SIGUSR1);
    (void)nanosleep(&settle, nullptr);
    (void)write(fds[1], "x", 1);
    status = -1;
    waitpid(reader, &status, 0);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "an ignored signal: read() doesn't notice");
    signal(SIGUSR1, SIG_DFL);

    // A full pipe.
    action.sa_handler = note_handled;
    action.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &action, nullptr);
    int full[2];
    (void)pipe(full);
    fcntl(full[1], F_SETFL, O_NONBLOCK);
    static char chunk[4096];
    while (write(full[1], chunk, sizeof(chunk)) > 0) { }
    fcntl(full[1], F_SETFL, 0);
    pid_t drainer = fork();
    if (drainer == 0) {
        close(full[1]);
        timespec a_while { 0, 60 * 1000 * 1000 };
        (void)nanosleep(&a_while, nullptr);
        while (read(full[0], chunk, sizeof(chunk)) > 0) { }
        _exit(0);
    }
    s_restart_handled = 0;
    soon_alarm();
    errno = 0;
    REQUIRE(write(full[1], chunk, 1) == 1, "write() to a full pipe, with SA_RESTART: it carries on");
    REQUIRE(s_restart_handled == 1, "and the handler ran");
    close(full[1]);
    waitpid(drainer, nullptr, 0);
    close(full[0]);

    action.sa_flags = 0;
    sigaction(SIGALRM, &action, nullptr);
    int another[2];
    (void)pipe(another);
    fcntl(another[1], F_SETFL, O_NONBLOCK);
    while (write(another[1], chunk, sizeof(chunk)) > 0) { }
    fcntl(another[1], F_SETFL, 0);
    soon_alarm();
    errno = 0;
    REQUIRE(write(another[1], chunk, 1) < 0 && errno == EINTR, "write() to a full pipe, with a handler: EINTR");
    close(another[0]);
    close(another[1]);

    // What doesn't get made again, whatever it was told.
    action.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &action, nullptr);
    soon_alarm();
    timespec longer { 2, 0 };
    errno = 0;
    timespec left {};
    REQUIRE(nanosleep(&longer, &left) < 0 && errno == EINTR, "nanosleep(), even with SA_RESTART: EINTR");
    REQUIRE(left.tv_sec >= 1, "and says how much was left");
    soon_alarm();
    REQUIRE(sleep(10) >= 9, "sleep() says how much it didn't get");
    pollfd waiting { fds[0], POLLIN, 0 };
    soon_alarm();
    errno = 0;
    REQUIRE(poll(&waiting, 1, 2000) < 0 && errno == EINTR, "poll(), even with SA_RESTART: EINTR");
    soon_alarm();
    errno = 0;
    REQUIRE(pause() < 0 && errno == EINTR, "pause(), even with SA_RESTART: EINTR");

    // A child's end.
    action.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &action, nullptr);
    pid_t waited_for = fork();
    if (waited_for == 0) {
        timespec a_while { 0, 60 * 1000 * 1000 };
        (void)nanosleep(&a_while, nullptr);
        _exit(3);
    }
    soon_alarm();
    REQUIRE(waitpid(waited_for, &status, 0) == waited_for && WEXITSTATUS(status) == 3, "waitpid(), with SA_RESTART: it carries on");
    action.sa_flags = 0;
    sigaction(SIGALRM, &action, nullptr);
    waited_for = fork();
    if (waited_for == 0) {
        timespec a_while { 0, 60 * 1000 * 1000 };
        (void)nanosleep(&a_while, nullptr);
        _exit(3);
    }
    soon_alarm();
    errno = 0;
    REQUIRE(waitpid(waited_for, &status, 0) < 0 && errno == EINTR, "waitpid(), with a handler: EINTR");
    REQUIRE(waitpid(waited_for, &status, 0) == waited_for, "and it's still there to wait for");

    // A socket.
    int pair[2];
    (void)socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    action.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &action, nullptr);
    writer = write_later(pair[1], 60);
    soon_alarm();
    REQUIRE(recv(pair[0], &byte, 1, 0) == 1 && byte == '!', "recv(), with SA_RESTART: it carries on");
    waitpid(writer, nullptr, 0);
    action.sa_flags = 0;
    sigaction(SIGALRM, &action, nullptr);
    soon_alarm();
    errno = 0;
    REQUIRE(recv(pair[0], &byte, 1, 0) < 0 && errno == EINTR, "recv(), with a handler: EINTR");
    close(pair[0]);
    close(pair[1]);

    signal(SIGALRM, SIG_DFL);
    close(fds[0]);
    close(fds[1]);
    close(report[0]);
    close(report[1]);
    end_area();
}

void note_cleanup(void* argument)
{
    *static_cast<int*>(argument) = 1;
}

struct CancelledWhileWaiting {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    int cleaned_up { 0 };
    int had_mutex { 0 };
};

void note_mutex_held(void* argument)
{
    auto* state = static_cast<CancelledWhileWaiting*>(argument);
    // That this thread has it is what POSIX promises, whatever it was in the middle of.
    state->had_mutex = pthread_mutex_trylock(&state->mutex) != 0;
    pthread_mutex_unlock(&state->mutex);
    state->cleaned_up = 1;
}

void short_pause()
{
    timespec settle { 0, 30 * 1000 * 1000 };
    (void)nanosleep(&settle, nullptr);
}

void cancellation()
{
    begin("cancellation");
    pthread_t thread;
    void* result = nullptr;
    int fds[2];
    (void)pipe(fds);
    static int s_fd;
    s_fd = fds[0];

    // In read(), for ever.
    static int s_cleaned;
    s_cleaned = 0;
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        pthread_cleanup_push(note_cleanup, &s_cleaned);
        char byte;
        (void)read(s_fd, &byte, 1);
        pthread_cleanup_pop(0);
        return nullptr;
    }, nullptr) == 0, "a thread waiting in read()");
    short_pause();
    REQUIRE(pthread_cancel(thread) == 0, "pthread_cancel()");
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "read() is where it ends: PTHREAD_CANCELED");
    REQUIRE(s_cleaned == 1, "and its cleanup handler ran");

    // Sleeping.
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        timespec a_long_time { 1000, 0 };
        (void)nanosleep(&a_long_time, nullptr);
        return reinterpret_cast<void*>(1234);
    }, nullptr) == 0, "a thread in nanosleep()");
    short_pause();
    pthread_cancel(thread);
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "nanosleep() is a cancellation point");

    // poll().
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        pollfd waiting { s_fd, POLLIN, 0 };
        (void)poll(&waiting, 1, -1);
        return reinterpret_cast<void*>(1234);
    }, nullptr) == 0, "a thread in poll()");
    short_pause();
    pthread_cancel(thread);
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "poll() is a cancellation point");

    // Not while it's not waiting: until it gets to one.
    static volatile int s_go;
    s_go = 0;
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        while (!s_go) { }
        pthread_testcancel();
        return reinterpret_cast<void*>(1234);
    }, nullptr) == 0, "a thread that isn't at one");
    pthread_cancel(thread);
    short_pause();
    s_go = 1;
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "pthread_testcancel() is where it ends");

    // With it turned off, nothing.
    s_go = 0;
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        int previous = -1;
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
        while (!s_go) {
            timespec a_while { 0, 1000 * 1000 };
            (void)nanosleep(&a_while, nullptr);
            pthread_testcancel();
        }
        (void)pthread_setcancelstate(previous, nullptr);
        return reinterpret_cast<void*>(1234);
    }, nullptr) == 0, "a thread that won't have it");
    pthread_cancel(thread);
    short_pause();
    s_go = 1;
    REQUIRE(pthread_join(thread, &result) == 0 && result == reinterpret_cast<void*>(1234), "PTHREAD_CANCEL_DISABLE: it just carries on");

    // And it's kept for when it's turned on.
    s_go = 0;
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr);
        while (!s_go) {
            timespec a_while { 0, 1000 * 1000 };
            (void)nanosleep(&a_while, nullptr);
        }
        (void)pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
        pthread_testcancel();
        return reinterpret_cast<void*>(1234);
    }, nullptr) == 0, "a thread that'll have it later");
    pthread_cancel(thread);
    short_pause();
    s_go = 1;
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "and it comes when it's turned back on");

    // Any time at all: not at any of the places it could happen otherwise.
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        int previous = -1;
        (void)pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &previous);
        timespec started, now;
        clock_gettime(CLOCK_MONOTONIC, &started);
        do {
            for (volatile int i = 0; i < 1000; i = i + 1) { }
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (now.tv_sec - started.tv_sec < 3);
        return reinterpret_cast<void*>(1234);
    }, nullptr) == 0, "a thread that keeps going");
    short_pause();
    pthread_cancel(thread);
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "PTHREAD_CANCEL_ASYNCHRONOUS: it goes anyway");

    // Waiting on a condition: it has the mutex again by then.
    static CancelledWhileWaiting s_state;
    s_state.cleaned_up = 0;
    s_state.had_mutex = 0;
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        pthread_mutex_lock(&s_state.mutex);
        pthread_cleanup_push(note_mutex_held, &s_state);
        for (;;)
            pthread_cond_wait(&s_state.condition, &s_state.mutex);
        pthread_cleanup_pop(0);
        return nullptr;
    }, nullptr) == 0, "a thread waiting on a condition");
    short_pause();
    pthread_cancel(thread);
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "pthread_cond_wait() is a cancellation point");
    REQUIRE(s_state.cleaned_up == 1, "and it cleans up");
    REQUIRE(pthread_mutex_trylock(&s_state.mutex) == 0, "and the mutex is free again, once that's done");
    pthread_mutex_unlock(&s_state.mutex);

    // Waiting for someone else.
    static pthread_t s_other;
    s_go = 0;
    (void)pthread_create(&s_other, nullptr, [](void*) -> void* {
        while (!s_go) {
            timespec a_while { 0, 1000 * 1000 };
            (void)nanosleep(&a_while, nullptr);
        }
        return reinterpret_cast<void*>(77);
    }, nullptr);
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        (void)pthread_join(s_other, nullptr);
        return reinterpret_cast<void*>(1234);
    }, nullptr) == 0, "a thread in pthread_join()");
    short_pause();
    pthread_cancel(thread);
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "pthread_join() is a cancellation point");
    s_go = 1;
    REQUIRE(pthread_join(s_other, &result) == 0 && result == reinterpret_cast<void*>(77), "the one it was joining is still joinable");

    // Files keep working, for everyone else: what a thread was doing with one doesn't leave them locked.
    s_go = 0;
    REQUIRE(pthread_create(&thread, nullptr, [](void*) -> void* {
        for (;;) {
            FILE* file = fopen("/dev/null", "w");
            if (file) {
                fputs("to nobody\n", file);
                fclose(file);
            }
            pthread_testcancel();
        }
        return nullptr;
    }, nullptr) == 0, "a thread using files");
    short_pause();
    pthread_cancel(thread);
    REQUIRE(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "cancelled in the middle of using them");
    FILE* afterwards = fopen("/dev/null", "w");
    REQUIRE(afterwards && fputs("still working\n", afterwards) >= 0 && fclose(afterwards) == 0, "files still work");

    errno = 0;
    int state_previous = 0;
    REQUIRE(pthread_setcancelstate(99, &state_previous) == EINVAL, "pthread_setcancelstate() of nothing: EINVAL");
    REQUIRE(pthread_setcanceltype(99, &state_previous) == EINVAL, "pthread_setcanceltype() of nothing: EINVAL");
    close(fds[0]);
    close(fds[1]);
    end_area();
}

void shared_locks()
{
    begin("shared locks");
    struct Shared {
        pthread_mutex_t mutex;
        pthread_cond_t condition;
        int ready;
        int count;
    };
    auto* shared = static_cast<Shared*>(mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    REQUIRE(shared != MAP_FAILED, "memory to share");
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    int value = -1;
    REQUIRE(pthread_mutexattr_getpshared(&attributes, &value) == 0 && value == PTHREAD_PROCESS_PRIVATE, "pthread_mutexattr_getpshared(): private to start with");
    REQUIRE(pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) == 0, "pthread_mutexattr_setpshared()");
    REQUIRE(pthread_mutexattr_getpshared(&attributes, &value) == 0 && value == PTHREAD_PROCESS_SHARED, "and it says so");
    REQUIRE(pthread_mutexattr_setpshared(&attributes, 99) == EINVAL, "pthread_mutexattr_setpshared() of nothing: EINVAL");
    REQUIRE(pthread_mutex_init(&shared->mutex, &attributes) == 0, "pthread_mutex_init(), between processes");
    pthread_condattr_t condition_attributes;
    pthread_condattr_init(&condition_attributes);
    REQUIRE(pthread_condattr_setpshared(&condition_attributes, PTHREAD_PROCESS_SHARED) == 0, "pthread_condattr_setpshared()");
    REQUIRE(pthread_cond_init(&shared->condition, &condition_attributes) == 0, "pthread_cond_init(), between processes");

    // Each gets it in turn, and nothing's lost.
    constexpr int rounds = 2000;
    constexpr int workers = 3;
    pid_t children[workers];
    for (int i = 0; i < workers; ++i) {
        children[i] = fork();
        if (children[i] == 0) {
            for (int round = 0; round < rounds; ++round) {
                pthread_mutex_lock(&shared->mutex);
                int const seen = shared->count;
                if (round % 64 == 0)
                    sched_yield();
                shared->count = seen + 1;
                pthread_mutex_unlock(&shared->mutex);
            }
            _exit(0);
        }
    }
    for (int i = 0; i < workers; ++i)
        waitpid(children[i], nullptr, 0);
    REQUIRE(shared->count == workers * rounds, "a mutex between processes keeps them out of each other's way");

    // One waits for another to say so.
    pid_t waiter = fork();
    if (waiter == 0) {
        pthread_mutex_lock(&shared->mutex);
        while (!shared->ready)
            pthread_cond_wait(&shared->condition, &shared->mutex);
        pthread_mutex_unlock(&shared->mutex);
        _exit(42);
    }
    short_pause();
    pthread_mutex_lock(&shared->mutex);
    shared->ready = 1;
    pthread_cond_broadcast(&shared->condition);
    pthread_mutex_unlock(&shared->mutex);
    int status = 0;
    waitpid(waiter, &status, 0);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 42, "a condition between processes wakes the other");

    // A mutex held by a process is held, even in one that started out the same.
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_destroy(&shared->mutex);
    REQUIRE(pthread_mutex_init(&shared->mutex, &attributes) == 0, "one that checks who has it");
    pthread_mutex_lock(&shared->mutex);
    pid_t other = fork();
    if (other == 0)
        _exit(pthread_mutex_trylock(&shared->mutex) == EBUSY ? 0 : 1);
    waitpid(other, &status, 0);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "held by a process, it's busy for another");
    pthread_mutex_unlock(&shared->mutex);

    // Named semaphores, which are what a Mac has for this.
    char name[64];
    snprintf(name, sizeof(name), "/vulture-posix-sem-%d", getpid());
    sem_unlink(name);
    errno = 0;
    REQUIRE(sem_open(name, 0) == SEM_FAILED && errno == ENOENT, "sem_open() of nothing: ENOENT");
    sem_t* semaphore = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    REQUIRE(semaphore != SEM_FAILED, "sem_open(O_CREAT)");
    errno = 0;
    REQUIRE(sem_open(name, O_CREAT | O_EXCL, 0600, 1) == SEM_FAILED && errno == EEXIST, "sem_open(O_EXCL) of what's there: EEXIST");
    REQUIRE(sem_trywait(semaphore) == 0, "sem_trywait() takes what's there");
    errno = 0;
    REQUIRE(sem_trywait(semaphore) < 0 && errno == EAGAIN, "and with nothing there: EAGAIN");
    pid_t poster = fork();
    if (poster == 0) {
        sem_t* mine = sem_open(name, 0);
        if (mine == SEM_FAILED)
            _exit(1);
        short_pause();
        sem_post(mine);
        sem_close(mine);
        _exit(0);
    }
    REQUIRE(sem_wait(semaphore) == 0, "sem_wait() until another process posts");
    waitpid(poster, &status, 0);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "it found it by name");
    sem_t* same = sem_open(name, O_CREAT, 0600, 99);
    REQUIRE(same != SEM_FAILED, "sem_open(O_CREAT) of what's there");
    errno = 0;
    REQUIRE(sem_trywait(same) < 0 && errno == EAGAIN, "doesn't change its value");
    REQUIRE(sem_close(same) == 0, "sem_close()");
    REQUIRE(sem_close(semaphore) == 0, "sem_close() of the other");
    REQUIRE(sem_unlink(name) == 0, "sem_unlink()");
    errno = 0;
    REQUIRE(sem_unlink(name) < 0 && errno == ENOENT, "sem_unlink() of what's gone: ENOENT");
    pthread_mutexattr_destroy(&attributes);
    pthread_condattr_destroy(&condition_attributes);
    munmap(shared, sizeof(Shared));
    end_area();
}

// What's printed is the true value, rounded correctly; and what's read, the nearest number there is to it. These are what a Mac says.
void same_text(char const* format, double value, char const* expected, char const* what)
{
    char buffer[2048];
    snprintf(buffer, sizeof(buffer), format, value);
    REQUIRE(strcmp(buffer, expected) == 0, what);
    if (strcmp(buffer, expected) != 0)
        fprintf(stderr, "posix: %s of %g: %s, not %s\n", format, value, buffer, expected);
}

void same_number(char const* text, double expected, char const* what)
{
    char* end = nullptr;
    double const value = strtod(text, &end);
    unsigned long long a, b;
    memcpy(&a, &value, 8);
    memcpy(&b, &expected, 8);
    REQUIRE(a == b && *end == 0, what);
}

void numbers_as_text()
{
    begin("numbers as text");
    same_text("%f", 0.25, "0.250000", "%f");
    same_text("%f", 1.0 / 3.0, "0.333333", "%f of a third");
    same_text("%.0f", 0.5, "0", "%.0f of a half: to the even one");
    same_text("%.0f", 1.5, "2", "%.0f of one and a half: to the even one");
    same_text("%.0f", 2.5, "2", "%.0f of two and a half: to the even one");
    same_text("%.1f", 0.25, "0.2", "%.1f of a quarter: exactly half way");
    same_text("%.1f", 0.35, "0.3", "%.1f of 0.35: which isn't quite");
    same_text("%.2f", 1.005, "1.00", "%.2f of 1.005: which is a little less");
    same_text("%.20f", 0.1, "0.10000000000000000555", "%.20f of 0.1: what it really is");
    same_text("%.0f", 1e21, "1000000000000000000000", "%.0f of 1e21");
    same_text("%f", 1e15, "1000000000000000.000000", "%f of a large number");
    same_text("%.0f", 1e300, "1000000000000000052504760255204420248704468581108159154915854115511802457988908195786371375080447864043704443832883878176942523235360430575644792184786706982848387200926575803737830233794788090059368953234970799945081119038967640880074652742780142494579258788820056842838115669472196386865459400540160", "%.0f of 1e300: every digit of it");
    same_text("%.3f", 999.9995, "1000.000", "%.3f rounding up past a power of ten");
    same_text("%#.0f", 1.0, "1.", "%#.0f keeps the point");
    same_text("%08.3f", 3.14159, "0003.142", "%08.3f pads with zeros");
    same_text("%-8.3f|", 3.14159, "3.142   |", "%-8.3f pads on the right");
    same_text("%+.1f", 1.0, "+1.0", "%+f");
    same_text("% .1f", 1.0, " 1.0", "% f");
    same_text("%f", -0.0, "-0.000000", "%f of negative zero");
    same_text("%e", 1234.5, "1.234500e+03", "%e");
    same_text("%E", 1234.5, "1.234500E+03", "%E");
    same_text("%.2e", 1234.5, "1.23e+03", "%.2e");
    same_text("%.0e", 1500.0, "2e+03", "%.0e");
    same_text("%#.0e", 1500.0, "2.e+03", "%#.0e");
    same_text("%e", 0.0, "0.000000e+00", "%e of zero");
    same_text("%e", 1e-10, "1.000000e-10", "%e of something small");
    same_text("%e", 1e100, "1.000000e+100", "%e with three digits of exponent");
    same_text("%.3e", 9.9996, "1.000e+01", "%.3e rounding up into the next power");
    same_text("%g", 0.25, "0.25", "%g");
    same_text("%g", 100000.0, "100000", "%g of 100000");
    same_text("%g", 1000000.0, "1e+06", "%g of a million");
    same_text("%g", 0.0001, "0.0001", "%g of 0.0001");
    same_text("%g", 0.00001, "1e-05", "%g of 0.00001");
    same_text("%g", 0.0, "0", "%g of zero");
    same_text("%g", 123456789.0, "1.23457e+08", "%g of a lot of digits");
    same_text("%.3g", 3.14159, "3.14", "%.3g");
    same_text("%.0g", 3.14159, "3", "%.0g");
    same_text("%#g", 1.5, "1.50000", "%#g keeps its zeros");
    same_text("%G", 1e-10, "1E-10", "%G");
    same_text("%.17g", 0.1, "0.10000000000000001", "%.17g of 0.1");
    same_text("%.15g", 0.1, "0.1", "%.15g of 0.1");
    same_text("%g", 1e300, "1e+300", "%g of 1e300");
    same_text("%g", 5e-324, "4.94066e-324", "%g of the smallest there is");
    same_text("%g", 1.7976931348623157e308, "1.79769e+308", "%g of the largest there is");
    same_text("%.17g", 1.7976931348623157e308, "1.7976931348623157e+308", "%.17g of the largest");
    same_text("%a", 1.0, "0x1p+0", "%a of one");
    same_text("%a", 0.5, "0x1p-1", "%a of a half");
    same_text("%a", 3.5, "0x1.cp+1", "%a of 3.5");
    same_text("%A", 3.5, "0X1.CP+1", "%A");
    same_text("%a", 0.0, "0x0p+0", "%a of zero");
    same_text("%.2a", 1.0 / 3.0, "0x1.55p-2", "%.2a");
    same_text("%f", __builtin_inf(), "inf", "%f of infinity");
    same_text("%F", __builtin_inf(), "INF", "%F of infinity");
    same_text("%e", -__builtin_inf(), "-inf", "%e of minus infinity");
    same_text("%g", __builtin_nan(""), "nan", "%g of not a number");
    same_text("%5.1f", __builtin_inf(), "  inf", "%5.1f of infinity is padded");
    same_text("%05f", __builtin_inf(), "  inf", "%05f of infinity isn't padded with zeros");

    same_number("0.1", 0.1, "strtod() of 0.1");
    same_number("1e23", 1e23, "strtod() of 1e23: a hard one");
    same_number("2.2250738585072014e-308", 2.2250738585072014e-308, "strtod() of the smallest normal one");
    same_number("1.7976931348623157e308", 1.7976931348623157e308, "strtod() of the largest");
    same_number("5e-324", 5e-324, "strtod() of the smallest");
    same_number("2.4703282292062328e-324", 5e-324, "strtod() just over half of the smallest");
    same_number("2.4703282292062327e-324", 0.0, "strtod() just under half of the smallest");
    same_number("9007199254740993", 9007199254740992.0, "strtod() exactly half way: to the even one");
    same_number("9007199254740995", 9007199254740996.0, "strtod() exactly half way, the other way");
    same_number("9007199254740993.0000000000000000000000000000000000000001", 9007199254740994.0, "strtod() barely past half way");
    same_number("123456789012345678901234567890", 123456789012345678901234567890.0, "strtod() of a lot of digits");
    same_number("0.000000000000000000000000000000000000001", 1e-39, "strtod() of a lot of zeros");
    same_number("1.5", 1.5, "strtod() of 1.5");
    same_number(".5", 0.5, "strtod() with no whole part");
    same_number("5.", 5.0, "strtod() with no fraction");
    same_number("1e0", 1.0, "strtod() with an exponent of zero");
    same_number("1E+2", 100.0, "strtod() with E and a sign");
    same_number("-2.5", -2.5, "strtod() negative");
    same_number("+2.5", 2.5, "strtod() with a plus");
    same_number("  7", 7.0, "strtod() after white space");
    same_number("0x10", 16.0, "strtod() of hexadecimal");
    same_number("0x1.8p1", 3.0, "strtod() of hexadecimal with an exponent");
    same_number("0x.8", 0.5, "strtod() of a hexadecimal fraction");
    same_number("infinity", __builtin_inf(), "strtod() of infinity");
    same_number("-INF", -__builtin_inf(), "strtod() of -INF");
    char* end = nullptr;
    REQUIRE(strtod("nan", &end) != strtod("nan", &end) && *end == 0, "strtod() of nan");
    REQUIRE(strtod("nan(1)", &end) != 0 && *end == 0, "strtod() of nan(1)");
    REQUIRE(strtod("abc", &end) == 0 && strcmp(end, "abc") == 0, "strtod() of what isn't a number leaves it");
    REQUIRE(strtod("1.5abc", &end) == 1.5 && strcmp(end, "abc") == 0, "strtod() stops where the number does");
    REQUIRE(strtod("1e", &end) == 1 && strcmp(end, "e") == 0, "strtod() of an exponent with nothing in it");
    REQUIRE(strtod("0x", &end) == 0 && strcmp(end, "x") == 0, "strtod() of 0x with nothing after it");
    errno = 0;
    REQUIRE(strtod("1e999", &end) == __builtin_inf() && errno == ERANGE, "strtod() too large: ERANGE");
    errno = 0;
    REQUIRE(strtod("1e-999", &end) == 0 && errno == ERANGE, "strtod() too small: ERANGE");
    REQUIRE(strtof("0.1", &end) == 0.1f, "strtof() of 0.1");
    REQUIRE(strtof("16777217", &end) == 16777216.0f, "strtof() half way: to the even one");
    REQUIRE(strtof("1.17549435e-38", &end) == 1.17549435e-38f, "strtof() of the smallest normal one");
    double read_back = 0;
    char text[64];
    double const originals[] = { 0.1, 1.0 / 3.0, 1e23, 5e-324, 1.7976931348623157e308, 123456.789 };
    for (double const original : originals) {
        snprintf(text, sizeof(text), "%.17g", original);
        read_back = strtod(text, nullptr);
        REQUIRE(read_back == original, "%.17g and back is the same number");
    }
    end_area();
}

void dynamic_loading()
{
    begin("dynamic loading");
    (void)dlerror();
    void* self = dlopen(nullptr, RTLD_NOW);
    REQUIRE(self != nullptr, "dlopen(NULL) is the program");
    REQUIRE(dlerror() == nullptr, "no error, for that");
    auto* found = dlsym(RTLD_DEFAULT, "printf");
    REQUIRE(found == reinterpret_cast<void*>(printf), "dlsym(RTLD_DEFAULT) finds printf");
    REQUIRE(dlsym(self, "printf") != nullptr, "dlsym() of what the program uses");
    REQUIRE(dlclose(self) == 0, "dlclose() of the program");
    REQUIRE(dlsym(RTLD_DEFAULT, "no_such_symbol_at_all") == nullptr, "dlsym() of nothing");
    char const* error = dlerror();
    REQUIRE(error != nullptr && strstr(error, "no_such_symbol_at_all") != nullptr && strstr(error, "symbol not found") != nullptr, "dlerror() says which, and why");
    REQUIRE(dlerror() == nullptr, "dlerror() says it once");
    REQUIRE(dlopen("/no/such/plugin.dylib", RTLD_NOW) == nullptr, "dlopen() of nothing");
    error = dlerror();
    REQUIRE(error != nullptr && strstr(error, "/no/such/plugin.dylib") != nullptr, "dlerror() names it");
    write_file("not-a-library.dylib", "this isn't code");
    REQUIRE(dlopen(path("not-a-library.dylib"), RTLD_NOW) == nullptr, "dlopen() of what isn't a library");
    REQUIRE(dlerror() != nullptr, "and it says so");
    Dl_info info {};
    REQUIRE(dladdr(reinterpret_cast<void*>(printf), &info) != 0, "dladdr() of a function");
    REQUIRE(info.dli_fname != nullptr && info.dli_fbase != nullptr, "dladdr() knows where it's from");
    REQUIRE(info.dli_sname != nullptr && strcmp(info.dli_sname, "printf") == 0, "dladdr() knows its name");
    REQUIRE(info.dli_saddr == reinterpret_cast<void*>(printf), "dladdr() knows where it starts");
    int on_stack = 0;
    REQUIRE(dladdr(&on_stack, &info) == 0, "dladdr() of what's in no library");
    end_area();
}

void wide_characters()
{
    begin("wide characters");
    setlocale(LC_ALL, "en_US.UTF-8");
    REQUIRE(wcslen(L"abc") == 3, "wcslen()");
    wchar_t buffer[16];
    REQUIRE(wcscmp(wcscpy(buffer, L"xyz"), L"xyz") == 0, "wcscpy() and wcscmp()");
    REQUIRE(wcscmp(wcscat(buffer, L"w"), L"xyzw") == 0, "wcscat()");
    REQUIRE(wcsncmp(L"abcd", L"abcf", 3) == 0 && wcsncmp(L"abcd", L"abcf", 4) < 0, "wcsncmp()");
    REQUIRE(wcschr(L"abc", L'b') != nullptr && wcschr(L"abc", L'q') == nullptr, "wcschr()");
    REQUIRE(wcsrchr(L"abca", L'a') - L"abca" >= 0, "wcsrchr()");
    REQUIRE(wcsstr(L"needle in", L"in") != nullptr, "wcsstr()");
    REQUIRE(wcstol(L"123", nullptr, 10) == 123, "wcstol()");
    REQUIRE(wcstoul(L"ff", nullptr, 16) == 255, "wcstoul()");
    REQUIRE(wcstod(L"1.5", nullptr) == 1.5, "wcstod()");
    wchar_t* word_end = nullptr;
    (void)wcstol(L"7 rest", &word_end, 10);
    REQUIRE(word_end && *word_end == L' ', "wcstol() says where it stopped");

    wchar_t character = 0;
    mbstate_t state {};
    REQUIRE(mbrtowc(&character, "a", 1, &state) == 1 && character == L'a', "mbrtowc() of ascii");
    REQUIRE(mbrtowc(&character, "\xc3\xa9", 2, &state) == 2 && character == 0xe9, "mbrtowc() of two bytes");
    REQUIRE(mbrtowc(&character, "\xe2\x82\xac", 3, &state) == 3 && character == 0x20ac, "mbrtowc() of three");
    REQUIRE(mbrtowc(&character, "\xf0\x9f\x98\x80", 4, &state) == 4 && character == 0x1f600, "mbrtowc() of four");
    memset(&state, 0, sizeof(state));
    REQUIRE(mbrtowc(&character, "\xc3", 1, &state) == static_cast<size_t>(-2), "mbrtowc() of part of one");
    REQUIRE(mbrtowc(&character, "\xa9", 1, &state) == 1 && character == 0xe9, "and then the rest");
    memset(&state, 0, sizeof(state));
    errno = 0;
    REQUIRE(mbrtowc(&character, "\xff", 1, &state) == static_cast<size_t>(-1) && errno == EILSEQ, "mbrtowc() of nonsense: EILSEQ");
    memset(&state, 0, sizeof(state));
    REQUIRE(mbsinit(&state) != 0, "mbsinit() at the start");
    char bytes[8] = {};
    REQUIRE(wcrtomb(bytes, 0x20ac, &state) == 3 && memcmp(bytes, "\xe2\x82\xac", 3) == 0, "wcrtomb()");
    errno = 0;
    REQUIRE(wcrtomb(bytes, 0x110000, &state) == static_cast<size_t>(-1) && errno == EILSEQ, "wcrtomb() of nothing there is: EILSEQ");
    memset(&state, 0, sizeof(state));
    char const* text = "h\xc3\xa9";
    REQUIRE(mbstowcs(buffer, text, 16) == 2 && buffer[1] == 0xe9, "mbstowcs()");
    REQUIRE(wcstombs(bytes, L"\xe9x", 8) == 3 && memcmp(bytes, "\xc3\xa9x", 3) == 0, "wcstombs()");
    REQUIRE(mbtowc(&character, "\xc3\xa9", 2) == 2 && character == 0xe9, "mbtowc()");
    REQUIRE(mblen("\xe2\x82\xac", 3) == 3, "mblen()");
    REQUIRE(wctomb(bytes, 0xe9) == 2, "wctomb()");
    REQUIRE(btowc('a') == L'a' && wctob(L'a') == 'a', "btowc() and wctob()");
    REQUIRE(wctob(0x20ac) == EOF, "wctob() of what's more than a byte");

    REQUIRE(iswalpha(L'a') && !iswalpha(L'1'), "iswalpha()");
    REQUIRE(iswdigit(L'1') && !iswdigit(L'a'), "iswdigit()");
    REQUIRE(iswspace(L' ') && !iswspace(L'a'), "iswspace()");
    REQUIRE(iswupper(L'A') && !iswupper(L'a'), "iswupper()");
    REQUIRE(iswlower(L'a') && !iswlower(L'A'), "iswlower()");
    REQUIRE(iswpunct(L'!') && !iswpunct(L'a'), "iswpunct()");
    REQUIRE(iswalnum(L'a') && iswalnum(L'1') && !iswalnum(L'!'), "iswalnum()");
    REQUIRE(iswxdigit(L'f') && !iswxdigit(L'g'), "iswxdigit()");
    REQUIRE(towupper(L'a') == L'A' && towlower(L'A') == L'a', "towupper() and towlower()");
    wctype_t const alpha = wctype("alpha");
    REQUIRE(alpha != 0 && iswctype(L'x', alpha), "wctype() and iswctype()");
    REQUIRE(wctype("no such class") == 0, "wctype() of nothing");
    REQUIRE(wcwidth(L'a') == 1, "wcwidth()");
    REQUIRE(wcwidth(0x4e00) == 2, "wcwidth() of a wide one");
    REQUIRE(wcwidth(0) == 0, "wcwidth() of nothing");

    REQUIRE(wmemcmp(L"abc", L"abd", 2) == 0, "wmemcmp()");
    wchar_t copied[4];
    REQUIRE(wmemcpy(copied, L"abc", 3) == copied && copied[2] == L'c', "wmemcpy()");
    REQUIRE(wmemset(copied, L'z', 2) == copied && copied[1] == L'z', "wmemset()");
    REQUIRE(wmemchr(L"abc", L'c', 3) != nullptr, "wmemchr()");
    wchar_t printed[32];
    REQUIRE(swprintf(printed, 32, L"%d-%s", 5, "x") == 3 && wcscmp(printed, L"5-x") == 0, "swprintf()");

    // Other scripts' letters, as what they are. (Tests/Parity has the rest of Unicode, by a comparison of every code point.)
    REQUIRE(iswalpha(0x3b1) && iswlower(0x3b1) && !iswupper(0x3b1), "Greek small alpha is a lower-case letter");
    REQUIRE(iswalpha(0x391) && iswupper(0x391) && !iswlower(0x391), "Greek capital alpha is an upper-case one");
    REQUIRE(iswalpha(0x627) && !iswupper(0x627) && !iswlower(0x627), "Arabic alef is a letter with no case");
    REQUIRE(iswalpha(0x4e2d) && iswprint(0x4e2d) && iswgraph(0x4e2d), "a Chinese character is a letter");
    REQUIRE(!iswdigit(0x660) && !iswalpha(0x660) && iswgraph(0x660), "only ASCII's digits are digits");
    REQUIRE(!iswalpha(0x301) && iswprint(0x301), "a combining mark isn't a letter");
    REQUIRE(iswspace(0x3000) && !iswblank(0x3000) && iswprint(0x3000) && !iswgraph(0x3000), "an ideographic space is a space");
    REQUIRE(iswspace(0xa0) && !iswblank(0xa0), "and so's a no-break space");
    REQUIRE(iswcntrl(0x85) && !iswspace(0x85) && iswcntrl(0x200b) && !iswprint(0x200b), "what's not to be seen is a control");
    REQUIRE(iswpunct(0x3001) && iswpunct(0x2014) && !iswpunct(0x20ac) && iswpunct('$'), "punctuation, and only ASCII's symbols");
    REQUIRE(!iswprint(0x378) && !iswprint(0xfffe) && !iswprint(0xd800) && iswprint(0xe000), "what isn't assigned isn't printable");
    REQUIRE(towupper(0x3b1) == 0x391 && towlower(0x391) == 0x3b1, "towupper() and towlower() of Greek");
    REQUIRE(towupper(0xdf) == 0xdf && towupper(0x17f) == 'S' && towlower(0x130) == 'i', "and of what's odd");
    REQUIRE(towupper(0x44f) == 0x42f && towlower(0x42f) == 0x44f, "and of Cyrillic");
    REQUIRE(wcwidth(0x4e2d) == 2 && wcwidth(0xac00) == 2 && wcwidth(0x1f600) == 2, "wcwidth() of what takes two columns");
    REQUIRE(wcwidth('a') == 1 && wcwidth(0xe9) == 1 && wcwidth(0x3b1) == 1, "of what takes one");
    REQUIRE(wcwidth(0x301) == 0 && wcwidth(0x200b) == 0 && wcwidth(0x1160) == 0 && wcwidth(0x903) == 0, "of what takes none");
    REQUIRE(wcwidth(1) == -1 && wcwidth(0x378) == -1 && wcwidth(0xad) == -1, "and of what isn't to be printed");
    setlocale(LC_ALL, "C");
    end_area();
}

void floating_environment()
{
    begin("floating point");
    REQUIRE(feclearexcept(FE_ALL_EXCEPT) == 0, "feclearexcept()");
    REQUIRE(fetestexcept(FE_ALL_EXCEPT) == 0, "fetestexcept() of nothing");
    REQUIRE(feraiseexcept(FE_INVALID) == 0, "feraiseexcept()");
    REQUIRE(fetestexcept(FE_INVALID) == FE_INVALID, "fetestexcept() sees it");
    REQUIRE(fetestexcept(FE_OVERFLOW) == 0, "and not another");
    fexcept_t flags;
    REQUIRE(fegetexceptflag(&flags, FE_ALL_EXCEPT) == 0, "fegetexceptflag()");
    REQUIRE(feclearexcept(FE_ALL_EXCEPT) == 0 && fetestexcept(FE_INVALID) == 0, "feclearexcept() clears it");
    REQUIRE(fesetexceptflag(&flags, FE_ALL_EXCEPT) == 0 && fetestexcept(FE_INVALID) == FE_INVALID, "fesetexceptflag() puts it back");
    feclearexcept(FE_ALL_EXCEPT);
    volatile double zero = 0.0;
    volatile double one = 1.0;
    volatile double result = one / zero;
    (void)result;
    REQUIRE(fetestexcept(FE_DIVBYZERO), "dividing by zero raises FE_DIVBYZERO");
    feclearexcept(FE_ALL_EXCEPT);
    volatile double big = 1e308;
    result = big * big;
    REQUIRE(fetestexcept(FE_OVERFLOW), "too large a number raises FE_OVERFLOW");
    feclearexcept(FE_ALL_EXCEPT);
    int const was = fegetround();
    REQUIRE(was == FE_TONEAREST, "fegetround() starts at the nearest");
    REQUIRE(fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD, "fesetround(FE_UPWARD)");
    volatile double third = 1.0;
    volatile double up = third / 3.0;
    REQUIRE(fesetround(FE_DOWNWARD) == 0 && fegetround() == FE_DOWNWARD, "fesetround(FE_DOWNWARD)");
    volatile double down = third / 3.0;
    REQUIRE(up > down, "and it rounds that way");
    REQUIRE(fesetround(FE_TOWARDZERO) == 0 && fegetround() == FE_TOWARDZERO, "fesetround(FE_TOWARDZERO)");
    REQUIRE(fesetround(was) == 0, "fesetround() back");
    REQUIRE(fesetround(0x12345) != 0, "fesetround() of no such mode");
    fenv_t environment;
    REQUIRE(fegetenv(&environment) == 0, "fegetenv()");
    fesetround(FE_UPWARD);
    REQUIRE(fesetenv(&environment) == 0 && fegetround() == was, "fesetenv() puts it back");
    feraiseexcept(FE_INEXACT);
    REQUIRE(feholdexcept(&environment) == 0 && fetestexcept(FE_ALL_EXCEPT) == 0, "feholdexcept() clears, and keeps");
    feraiseexcept(FE_INVALID);
    REQUIRE(feupdateenv(&environment) == 0 && fetestexcept(FE_INEXACT) && fetestexcept(FE_INVALID), "feupdateenv() has both");
    feclearexcept(FE_ALL_EXCEPT);
    end_area();
}

void character_sets()
{
    begin("character sets");
    iconv_t convert = iconv_open("UTF-16BE", "UTF-8");
    REQUIRE(convert != reinterpret_cast<iconv_t>(-1), "iconv_open(UTF-8 to UTF-16BE)");
    char in_text[] = "a\xc3\xa9";
    char* in = in_text;
    size_t in_left = 3;
    char out_text[16] = {};
    char* out = out_text;
    size_t out_left = sizeof(out_text);
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == 0, "iconv()");
    REQUIRE(in_left == 0 && out_left == sizeof(out_text) - 4, "all of it, and how much that came to");
    REQUIRE(memcmp(out_text, "\0a\0\xe9", 4) == 0, "UTF-16BE, correctly");
    REQUIRE(iconv_close(convert) == 0, "iconv_close()");
    errno = 0;
    REQUIRE(iconv_open("NO-SUCH-THING", "UTF-8") == reinterpret_cast<iconv_t>(-1) && errno == EINVAL, "iconv_open() of nothing: EINVAL");

    convert = iconv_open("ISO-8859-1", "UTF-8");
    char latin_in[] = "\xc3\xa9";
    in = latin_in;
    in_left = 2;
    out = out_text;
    out_left = sizeof(out_text);
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == 0 && static_cast<unsigned char>(out_text[0]) == 0xe9, "UTF-8 to ISO-8859-1");
    char euro[] = "\xe2\x82\xac";
    in = euro;
    in_left = 3;
    errno = 0;
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == static_cast<size_t>(-1) && errno == EILSEQ, "what it can't hold: EILSEQ");
    iconv_close(convert);

    convert = iconv_open("UTF-8", "ISO-8859-1");
    char latin[] = "\xe9";
    in = latin;
    in_left = 1;
    out = out_text;
    out_left = 1;
    errno = 0;
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == static_cast<size_t>(-1) && errno == E2BIG, "no room: E2BIG");
    out_left = 8;
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == 0 && memcmp(out_text, "\xc3\xa9", 2) == 0, "with room, it works");
    iconv_close(convert);

    convert = iconv_open("UTF-16LE", "UTF-8");
    char emoji[] = "\xf0\x9f\x98\x80";
    in = emoji;
    in_left = 4;
    out = out_text;
    out_left = sizeof(out_text);
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == 0 && memcmp(out_text, "\x3d\xd8\x00\xde", 4) == 0, "UTF-16 surrogates");
    iconv_close(convert);
    convert = iconv_open("UTF-8", "UTF-8");
    char partial[] = "\xc3";
    in = partial;
    in_left = 1;
    out = out_text;
    out_left = sizeof(out_text);
    errno = 0;
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == static_cast<size_t>(-1) && errno == EINVAL, "part of a character: EINVAL");
    iconv_close(convert);

    // The older ways of writing text, there and back.
    struct Sample {
        char const* encoding;
        char const* bytes;
        char const* utf8;
    };
    Sample const samples[] = {
        { "ISO-8859-2", "\xb1\xe6", "\xc4\x85\xc4\x87" },
        { "ISO-8859-5", "\xd4\xde\xdc", "\xd0\xb4\xd0\xbe\xd0\xbc" },
        { "ISO-8859-7", "\xe1\xe2", "\xce\xb1\xce\xb2" },
        { "ISO-8859-15", "\xa4", "\xe2\x82\xac" },
        { "CP1250", "\xb9", "\xc4\x85" },
        { "CP1251", "\xe4\xee\xec", "\xd0\xb4\xd0\xbe\xd0\xbc" },
        { "CP1252", "\x80\xe9", "\xe2\x82\xac\xc3\xa9" },
        { "CP1256", "\xc7", "\xd8\xa7" },
        { "KOI8-R", "\xc4\xcf\xcd", "\xd0\xb4\xd0\xbe\xd0\xbc" },
        { "MACINTOSH", "\x8e\xa9", "\xc3\xa9\xc2\xa9" },
        { "SHIFT_JIS", "\x82\xa0\x88\x9f\xb1", "\xe3\x81\x82\xe4\xba\x9c\xef\xbd\xb1" },
        { "EUC-JP", "\xa4\xa2\xb0\xa1", "\xe3\x81\x82\xe4\xba\x9c" },
        { "EUC-KR", "\xb0\xa1\xc7\xd1", "\xea\xb0\x80\xed\x95\x9c" },
        { "GBK", "\xd6\xd0\xce\xc4", "\xe4\xb8\xad\xe6\x96\x87" },
        { "GB18030", "\xd6\xd0\x81\x30\x81\x30", "\xe4\xb8\xad\xc2\x80" },
        { "BIG5", "\xa4\xa4\xa4\xe5", "\xe4\xb8\xad\xe6\x96\x87" },
    };
    for (auto const& sample : samples) {
        char label[96];
        snprintf(label, sizeof(label), "%s to UTF-8", sample.encoding);
        char result[32] = {};
        iconv_t decoder = iconv_open("UTF-8", sample.encoding);
        char* from = const_cast<char*>(sample.bytes);
        size_t from_left = strlen(sample.bytes);
        char* into = result;
        size_t into_left = sizeof(result);
        REQUIRE(decoder != reinterpret_cast<iconv_t>(-1) && iconv(decoder, &from, &from_left, &into, &into_left) == 0 && strcmp(result, sample.utf8) == 0, label);
        iconv_close(decoder);

        snprintf(label, sizeof(label), "UTF-8 to %s", sample.encoding);
        memset(result, 0, sizeof(result));
        iconv_t encoder = iconv_open(sample.encoding, "UTF-8");
        from = const_cast<char*>(sample.utf8);
        from_left = strlen(sample.utf8);
        into = result;
        into_left = sizeof(result);
        REQUIRE(encoder != reinterpret_cast<iconv_t>(-1) && iconv(encoder, &from, &from_left, &into, &into_left) == 0 && strcmp(result, sample.bytes) == 0, label);
        iconv_close(encoder);
    }

    // What it can't say, and what isn't right.
    convert = iconv_open("SHIFT_JIS", "UTF-8");
    char arabic[] = "\xd8\xa7";
    in = arabic;
    in_left = 2;
    out = out_text;
    out_left = sizeof(out_text);
    errno = 0;
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == static_cast<size_t>(-1) && errno == EILSEQ, "Shift_JIS has no Arabic: EILSEQ");
    iconv_close(convert);
    convert = iconv_open("UTF-8", "EUC-KR");
    char half[] = "\xb0";
    in = half;
    in_left = 1;
    out = out_text;
    out_left = sizeof(out_text);
    errno = 0;
    REQUIRE(iconv(convert, &in, &in_left, &out, &out_left) == static_cast<size_t>(-1) && errno == EINVAL, "half of a two-byte character: EINVAL");
    iconv_close(convert);
    end_area();
}
