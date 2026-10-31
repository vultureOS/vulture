/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/random.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

using System::Syscall;

namespace {

template<typename T = int>
T check(long result)
{
    if (result < 0 && result > -4096) {
        errno = static_cast<int>(-result);
        return static_cast<T>(-1);
    }
    return static_cast<T>(result);
}

template<typename... Args>
long call(Syscall number, Args... arguments)
{
    return System::syscall(number, (long)(arguments)...);
}

}

static_assert(sizeof(struct pollfd) == sizeof(vulture_pollfd));
static_assert(sizeof(struct utsname) == sizeof(vulture_utsname));

extern "C" {

uid_t getuid(void) { return static_cast<uid_t>(call(Syscall::getuid)); }
uid_t geteuid(void) { return static_cast<uid_t>(call(Syscall::geteuid)); }
gid_t getgid(void) { return static_cast<gid_t>(call(Syscall::getgid)); }
gid_t getegid(void) { return static_cast<gid_t>(call(Syscall::getegid)); }
int setuid(uid_t uid) { return check(call(Syscall::setuid, uid)); }
int setgid(gid_t gid) { return check(call(Syscall::setgid, gid)); }
int seteuid(uid_t uid) { return check(call(Syscall::seteuid, uid)); }
int setegid(gid_t gid) { return check(call(Syscall::setegid, gid)); }
int getgroups(int size, gid_t list[]) { return check(call(Syscall::getgroups, size, list)); }
int setgroups(size_t size, gid_t const* list) { return check(call(Syscall::setgroups, size, list)); }
mode_t umask(mode_t mask) { return static_cast<mode_t>(call(Syscall::umask, mask)); }
int chmod(char const* path, mode_t mode) { return check(call(Syscall::chmod, path, mode)); }
int fchmod(int fd, mode_t mode) { return check(call(Syscall::fchmod, fd, mode)); }
int chown(char const* path, uid_t uid, gid_t gid) { return check(call(Syscall::chown, path, static_cast<int>(uid), static_cast<int>(gid), 0)); }
int lchown(char const* path, uid_t uid, gid_t gid) { return check(call(Syscall::chown, path, static_cast<int>(uid), static_cast<int>(gid), AT_SYMLINK_NOFOLLOW)); }
int fchown(int fd, uid_t uid, gid_t gid) { return check(call(Syscall::fchown, fd, static_cast<int>(uid), static_cast<int>(gid))); }
int link(char const* existing, char const* created) { return check(call(Syscall::link, existing, created)); }

int utimensat(int directory_fd, char const* path, struct timespec const times[2], int flags)
{
    return check(call(Syscall::utimensat, directory_fd, path, times, flags));
}

int futimens(int fd, struct timespec const times[2])
{
    return check(call(Syscall::futimens, fd, times));
}

int uname(struct utsname* name) { return check(call(Syscall::uname, name)); }

int poll(struct pollfd* descriptors, nfds_t count, int timeout)
{
    return check(call(Syscall::poll, descriptors, count, timeout));
}

ssize_t getrandom(void* buffer, size_t length, unsigned flags)
{
    return check<ssize_t>(call(Syscall::getrandom, buffer, length, flags));
}

int getentropy(void* buffer, size_t length)
{
    if (length > 256) {
        errno = EIO;
        return -1;
    }
    return getrandom(buffer, length, 0) == static_cast<ssize_t>(length) ? 0 : -1;
}

void arc4random_buf(void* buffer, size_t length)
{
    auto* bytes = static_cast<unsigned char*>(buffer);
    while (length > 0) {
        ssize_t got = getrandom(bytes, length, 0);
        if (got <= 0)
            continue;
        bytes += got;
        length -= static_cast<size_t>(got);
    }
}

unsigned arc4random(void)
{
    unsigned value;
    arc4random_buf(&value, sizeof(value));
    return value;
}

unsigned arc4random_uniform(unsigned bound)
{
    if (bound < 2)
        return 0;
    // Reject the top few values so all results are equally likely.
    unsigned minimum = -bound % bound;
    for (;;) {
        unsigned value = arc4random();
        if (value >= minimum)
            return value % bound;
    }
}

int sysctlbyname(char const* name, void* old_value, size_t* old_length, void const* new_value, size_t new_length)
{
    return check(call(Syscall::sysctl, name, strlen(name), old_value, old_length, new_value, new_length));
}

int gethostname(char* name, size_t size)
{
    size_t length = size;
    if (sysctlbyname("kern.hostname", name, &length, nullptr, 0) < 0)
        return -1;
    return 0;
}

int sethostname(char const* name, size_t size)
{
    return sysctlbyname("kern.hostname", nullptr, nullptr, name, size);
}

int reboot(int how)
{
    return check(call(Syscall::reboot, how));
}
}
