/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/vulture.h>
#include <unistd.h>
#include <wchar.h>

using System::argument;
using System::Syscall;

// What M16 added: each a system call, or made of them.

namespace {

template<typename T = long>
T check(long result)
{
    if (result < 0 && result > -4096) {
        errno = static_cast<int>(-result);
        return static_cast<T>(-1);
    }
    return static_cast<T>(result);
}

template<typename... Args>
long call(Syscall number, Args... args)
{
    return System::syscall(number, argument(args)...);
}

}

extern "C" {

// ---- Limits.

int getrlimit(int resource, struct rlimit* limit)
{
    return check<int>(call(Syscall::getrlimit, resource, limit));
}

int setrlimit(int resource, struct rlimit const* limit)
{
    return check<int>(call(Syscall::setrlimit, resource, limit));
}

int getdtablesize()
{
    rlimit limit {};
    getrlimit(RLIMIT_NOFILE, &limit);
    return static_cast<int>(limit.rlim_cur);
}

// ---- Several buffers at once.

ssize_t readv(int fd, struct iovec const* vectors, int count)
{
    return check<ssize_t>(call(Syscall::readv, fd, vectors, count));
}

ssize_t writev(int fd, struct iovec const* vectors, int count)
{
    return check<ssize_t>(call(Syscall::writev, fd, vectors, count));
}

// ---- Relative to a directory, rather than where the program is.

int openat(int directory, char const* path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    return check<int>(call(Syscall::openat, directory, path, flags, mode));
}

int mkdirat(int directory, char const* path, mode_t mode)
{
    return check<int>(call(Syscall::mkdirat, directory, path, mode));
}

int unlinkat(int directory, char const* path, int flags)
{
    return check<int>(call(Syscall::unlinkat, directory, path, flags));
}

int renameat(int from_directory, char const* from, int to_directory, char const* to)
{
    return check<int>(call(Syscall::renameat, from_directory, from, to_directory, to));
}

ssize_t readlinkat(int directory, char const* path, char* buffer, size_t size)
{
    return check<ssize_t>(call(Syscall::readlinkat, directory, path, buffer, size));
}

int symlinkat(char const* target, int directory, char const* path)
{
    return check<int>(call(Syscall::symlinkat, target, directory, path));
}

int linkat(int from_directory, char const* from, int to_directory, char const* to, int flags)
{
    return check<int>(call(Syscall::linkat, from_directory, from, to_directory, to, flags));
}

int faccessat(int directory, char const* path, int mode, int flags)
{
    return check<int>(call(Syscall::faccessat, directory, path, mode, flags));
}

int fchmodat(int directory, char const* path, mode_t mode, int flags)
{
    return check<int>(call(Syscall::fchmodat, directory, path, mode, flags));
}

int fchownat(int directory, char const* path, uid_t owner, gid_t group, int flags)
{
    return check<int>(call(Syscall::fchownat, directory, path, owner, group, flags));
}

int fchdir(int fd)
{
    return check<int>(call(Syscall::fchdir, fd));
}

// ---- Named pipes.

int mknod(char const* path, mode_t mode, dev_t)
{
    return check<int>(call(Syscall::mknod, path, mode));
}

int mkfifo(char const* path, mode_t mode)
{
    return mknod(path, (mode & 07777) | S_IFIFO, 0);
}

// ---- select, by way of poll.

namespace {

int select_with(int count, fd_set* read_set, fd_set* write_set, fd_set* error_set, int timeout_milliseconds)
{
    if (count < 0 || count > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    pollfd descriptors[FD_SETSIZE];
    int used = 0;
    for (int fd = 0; fd < count; ++fd) {
        short events = 0;
        if (read_set && FD_ISSET(fd, read_set))
            events |= POLLIN;
        if (write_set && FD_ISSET(fd, write_set))
            events |= POLLOUT;
        if (error_set && FD_ISSET(fd, error_set))
            events |= POLLPRI;
        if (!events)
            continue;
        descriptors[used].fd = fd;
        descriptors[used].events = events;
        descriptors[used].revents = 0;
        ++used;
    }
    int const ready = poll(descriptors, static_cast<nfds_t>(used), timeout_milliseconds);
    if (ready < 0)
        return -1;
    if (read_set)
        FD_ZERO(read_set);
    if (write_set)
        FD_ZERO(write_set);
    if (error_set)
        FD_ZERO(error_set);
    int total = 0;
    for (int i = 0; i < used; ++i) {
        auto const& descriptor = descriptors[i];
        if (descriptor.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        if (read_set && (descriptor.events & POLLIN) && (descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(descriptor.fd, read_set);
            ++total;
        }
        if (write_set && (descriptor.events & POLLOUT) && (descriptor.revents & (POLLOUT | POLLHUP | POLLERR))) {
            FD_SET(descriptor.fd, write_set);
            ++total;
        }
        if (error_set && (descriptor.events & POLLPRI) && (descriptor.revents & POLLPRI)) {
            FD_SET(descriptor.fd, error_set);
            ++total;
        }
    }
    return total;
}

}

int select(int count, fd_set* read_set, fd_set* write_set, fd_set* error_set, struct timeval* timeout)
{
    int milliseconds = -1;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0) {
            errno = EINVAL;
            return -1;
        }
        milliseconds = static_cast<int>(timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000);
    }
    return select_with(count, read_set, write_set, error_set, milliseconds);
}

int pselect(int count, fd_set* read_set, fd_set* write_set, fd_set* error_set, struct timespec const* timeout, sigset_t const* mask)
{
    int milliseconds = -1;
    if (timeout)
        milliseconds = static_cast<int>(timeout->tv_sec * 1000 + (timeout->tv_nsec + 999'999) / 1'000'000);
    sigset_t previous;
    if (mask)
        sigprocmask(SIG_SETMASK, mask, &previous);
    int const result = select_with(count, read_set, write_set, error_set, milliseconds);
    if (mask) {
        int const saved = errno;
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        errno = saved;
    }
    return result;
}

// ---- kqueue.

int kqueue()
{
    return check<int>(call(Syscall::kqueue));
}

int kevent(int queue, struct kevent const* changes, int change_count, struct kevent* events, int event_count, struct timespec const* timeout)
{
    return check<int>(call(Syscall::kevent, queue, changes, change_count, events, event_count, timeout));
}

// ---- Sockets: Unix-domain ones, and messages with more than one buffer.

int socketpair(int domain, int type, int protocol, int fds[2])
{
    return check<int>(call(Syscall::socketpair, domain, type, protocol, fds));
}

ssize_t sendmsg(int fd, struct msghdr const* message, int flags)
{
    return check<ssize_t>(call(Syscall::sendmsg, fd, message, flags));
}

ssize_t recvmsg(int fd, struct msghdr* message, int flags)
{
    return check<ssize_t>(call(Syscall::recvmsg, fd, message, flags));
}

int getpeereid(int fd, uid_t* uid, gid_t* gid)
{
    xucred credentials {};
    socklen_t size = sizeof(credentials);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &credentials, &size) != 0)
        return -1;
    *uid = credentials.cr_uid;
    *gid = credentials.cr_groups[0];
    return 0;
}

// ---- Memory.

int madvise(void* address, size_t length, int advice)
{
    return check<int>(call(Syscall::madvise, address, length, advice));
}

int posix_madvise(void* address, size_t length, int advice)
{
    return madvise(address, length, advice) == 0 ? 0 : errno;
}

// Named shared memory is a file on the temporary filesystem, out of the way. Names are flat: a '/' to start is all they have.
namespace {

bool shared_memory_path(char const* name, char* path, size_t size)
{
    if (name[0] == '/')
        ++name;
    if (!name[0] || strchr(name, '/') || strlen(name) > 255) {
        errno = strchr(name, '/') ? EINVAL : ENAMETOOLONG;
        return false;
    }
    snprintf(path, size, "/tmp/.shared-memory/%s", name);
    return true;
}

}

int shm_open(char const* name, int flags, mode_t mode)
{
    char path[512];
    if (!shared_memory_path(name, path, sizeof(path)))
        return -1;
    // Anyone may make one, as with a file in /tmp.
    (void)mkdir("/tmp/.shared-memory", 01777);
    return open(path, flags | O_CLOEXEC, mode);
}

int shm_unlink(char const* name)
{
    char path[512];
    if (!shared_memory_path(name, path, sizeof(path)))
        return -1;
    return unlink(path);
}

// ---- Interval timers.

int setitimer(int which, struct itimerval const* value, struct itimerval* previous)
{
    return check<int>(call(Syscall::setitimer, which, value, previous));
}

int getitimer(int which, struct itimerval* value)
{
    return check<int>(call(Syscall::getitimer, which, value));
}

// ---- How busy it's been.

int getloadavg(double* averages, int count)
{
    if (count < 0)
        return -1;
    unsigned long long hundredths[3] = {};
    size_t size = sizeof(hundredths);
    if (sysctlbyname("vm.loadavg", hundredths, &size, nullptr, 0) != 0)
        return -1;
    int const answered = count > 3 ? 3 : count;
    for (int i = 0; i < answered; ++i)
        averages[i] = static_cast<double>(hundredths[i]) / 1000.0;
    return answered;
}

// ---- Debugging.

int ptrace(int request, pid_t pid, void* address, int data)
{
    return check<int>(call(Syscall::ptrace, request, pid, address, data));
}

// ---- Volumes.

int mount(char const* type, char const* directory, int flags, void* data)
{
    return check<int>(call(Syscall::mount, type, directory, flags, data));
}

int unmount(char const* directory, int flags)
{
    return check<int>(call(Syscall::unmount, directory, flags));
}

// ---- stdlib's side of wide characters.

int mbtowc(wchar_t* out, char const* text, size_t length)
{
    if (!text)
        return 0;
    mbstate_t state {};
    size_t const used = mbrtowc(out, text, length, &state);
    if (used == static_cast<size_t>(-1) || used == static_cast<size_t>(-2)) {
        errno = EILSEQ;
        return -1;
    }
    return static_cast<int>(used);
}

int mblen(char const* text, size_t length)
{
    return mbtowc(nullptr, text, length);
}

int wctomb(char* out, wchar_t character)
{
    if (!out)
        return 0;
    size_t const used = wcrtomb(out, character, nullptr);
    return used == static_cast<size_t>(-1) ? -1 : static_cast<int>(used);
}

size_t mbstowcs(wchar_t* out, char const* text, size_t count)
{
    mbstate_t state {};
    return mbsrtowcs(out, &text, count, &state);
}

size_t wcstombs(char* out, wchar_t const* text, size_t count)
{
    mbstate_t state {};
    return wcsrtombs(out, &text, count, &state);
}

}
