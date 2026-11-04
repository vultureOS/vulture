/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/acl.h>
#include <sys/clonefile.h>
#include <sys/snapshot.h>
#include <sys/stat.h>
#include <sys/xattr.h>

// What a file has besides its contents, and what the volume can do with it.

using System::Syscall;

namespace {

template<typename T>
T check(long result)
{
    if (result < 0) {
        errno = static_cast<int>(-result);
        return static_cast<T>(-1);
    }
    return static_cast<T>(result);
}

constexpr unsigned long operation(int which, int options) { return static_cast<unsigned long>(which | (options << 8)); }

template<typename... Arguments>
long call(Syscall number, Arguments... arguments)
{
    return System::syscall(number, (unsigned long)(arguments)...);
}

}

extern "C" {

ssize_t getxattr(char const* path, char const* name, void* value, size_t size, unsigned int position, int options)
{
    if (position) {
        errno = EINVAL;
        return -1;
    }
    return check<ssize_t>(call(Syscall::xattr, operation(VULTURE_XATTR_GET, options), -1, path, name, value, size));
}

ssize_t fgetxattr(int fd, char const* name, void* value, size_t size, unsigned int position, int options)
{
    if (position) {
        errno = EINVAL;
        return -1;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return check<ssize_t>(call(Syscall::xattr, operation(VULTURE_XATTR_GET, options), fd, nullptr, name, value, size));
}

int setxattr(char const* path, char const* name, void const* value, size_t size, unsigned int position, int options)
{
    if (position) {
        errno = EINVAL;
        return -1;
    }
    return check<int>(call(Syscall::xattr, operation(VULTURE_XATTR_SET, options), -1, path, name, value, size));
}

int fsetxattr(int fd, char const* name, void const* value, size_t size, unsigned int position, int options)
{
    if (position) {
        errno = EINVAL;
        return -1;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return check<int>(call(Syscall::xattr, operation(VULTURE_XATTR_SET, options), fd, nullptr, name, value, size));
}

int removexattr(char const* path, char const* name, int options)
{
    return check<int>(call(Syscall::xattr, operation(VULTURE_XATTR_REMOVE, options), -1, path, name, nullptr, 0));
}

int fremovexattr(int fd, char const* name, int options)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return check<int>(call(Syscall::xattr, operation(VULTURE_XATTR_REMOVE, options), fd, nullptr, name, nullptr, 0));
}

ssize_t listxattr(char const* path, char* names, size_t size, int options)
{
    return check<ssize_t>(call(Syscall::xattr, operation(VULTURE_XATTR_LIST, options), -1, path, nullptr, names, size));
}

ssize_t flistxattr(int fd, char* names, size_t size, int options)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return check<ssize_t>(call(Syscall::xattr, operation(VULTURE_XATTR_LIST, options), fd, nullptr, nullptr, names, size));
}

int clonefile(char const* source, char const* destination, unsigned int flags)
{
    return check<int>(call(Syscall::clonefile, AT_FDCWD, source, AT_FDCWD, destination, flags));
}

int clonefileat(int source_directory, char const* source, int destination_directory, char const* destination, unsigned int flags)
{
    return check<int>(call(Syscall::clonefile, source_directory, source, destination_directory, destination, flags));
}

int fclonefileat(int source_fd, int destination_directory, char const* destination, unsigned int flags)
{
    return check<int>(call(Syscall::clonefile, source_fd, nullptr, destination_directory, destination, flags));
}

int chflags(char const* path, unsigned int flags)
{
    return check<int>(call(Syscall::chflags, -1, path, flags));
}

int fchflags(int fd, unsigned int flags)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return check<int>(call(Syscall::chflags, fd, nullptr, flags));
}

int fs_snapshot_create(int fd, char const* name, unsigned int flags)
{
    return check<int>(call(Syscall::fs_snapshot, VULTURE_SNAPSHOT_CREATE, fd, name, 0, flags));
}

int fs_snapshot_delete(int fd, char const* name, unsigned int flags)
{
    return check<int>(call(Syscall::fs_snapshot, VULTURE_SNAPSHOT_DELETE, fd, name, 0, flags));
}

int fs_snapshot_revert(int fd, char const* name, unsigned int flags)
{
    return check<int>(call(Syscall::fs_snapshot, VULTURE_SNAPSHOT_REVERT, fd, name, 0, flags));
}

ssize_t fs_snapshot_list(int fd, char* names, size_t size, unsigned int flags)
{
    return check<ssize_t>(call(Syscall::fs_snapshot, VULTURE_SNAPSHOT_LIST, fd, names, size, flags));
}

int vulture_acl_get(char const* path, struct vulture_acl_entry* entries, unsigned int room, int follow)
{
    return check<int>(call(Syscall::acl, operation(VULTURE_ACL_GET, follow ? 0 : XATTR_NOFOLLOW), -1, path, entries, room));
}

int vulture_acl_set(char const* path, struct vulture_acl_entry const* entries, unsigned int count, int follow)
{
    return check<int>(call(Syscall::acl, operation(VULTURE_ACL_SET, follow ? 0 : XATTR_NOFOLLOW), -1, path, entries, count));
}

}
