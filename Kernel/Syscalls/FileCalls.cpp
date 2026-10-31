/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/Pipe.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

static int as_int(u64 value)
{
    return static_cast<int>(static_cast<i32>(value));
}

ErrorOr<FlatPtr> Process::sys_open(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    int flags = as_int(arguments.values[1]);
    auto description = TRY(VirtualFileSystem::open(path.view(), m_cwd.view(), flags, static_cast<u32>(arguments.values[2])));
    return static_cast<FlatPtr>(TRY(allocate_fd(move(description), flags & O_CLOEXEC)));
}

ErrorOr<FlatPtr> Process::sys_close(TrapFrame&, SyscallArguments const& arguments)
{
    int fd = as_int(arguments.values[0]);
    if (fd < 0 || static_cast<size_t>(fd) >= max_open_files)
        return Error::from_errno(EBADF);
    FileDescriptor closed;
    {
        SpinLockGuard guard(m_files_lock);
        if (!m_files[fd].description)
            return Error::from_errno(EBADF);
        closed = move(m_files[fd]);
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_read(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (!is_user_range(arguments.values[1], arguments.values[2]))
        return Error::from_errno(EFAULT);
    return TRY(description->read(IOBuffer::user(arguments.values[1]), arguments.values[2]));
}

ErrorOr<FlatPtr> Process::sys_write(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (arguments.values[2] == 0)
        return 0;
    if (!is_user_range(arguments.values[1], arguments.values[2]))
        return Error::from_errno(EFAULT);
    return TRY(description->write(IOBuffer::user(arguments.values[1]), arguments.values[2]));
}

ErrorOr<FlatPtr> Process::sys_lseek(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    return static_cast<FlatPtr>(TRY(description->seek(static_cast<i64>(arguments.values[1]), as_int(arguments.values[2]))));
}

ErrorOr<FlatPtr> Process::sys_fstat(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    TRY(copy_to_user(arguments.values[1], TRY(description->stat())));
    return 0;
}

static ErrorOr<FlatPtr> stat_path(StringView path, StringView cwd, FlatPtr result, bool follow)
{
    auto resolved = TRY(VirtualFileSystem::resolve(path, cwd, follow));
    TRY(copy_to_user(result, resolved.inode->metadata().to_stat(resolved.inode->file_system().id())));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_stat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    return stat_path(path.view(), m_cwd.view(), arguments.values[1], true);
}

ErrorOr<FlatPtr> Process::sys_lstat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    return stat_path(path.view(), m_cwd.view(), arguments.values[1], false);
}

ErrorOr<FlatPtr> Process::sys_dup(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    return static_cast<FlatPtr>(TRY(allocate_fd(move(description), false)));
}

ErrorOr<FlatPtr> Process::sys_dup2(TrapFrame&, SyscallArguments const& arguments)
{
    int old_fd = as_int(arguments.values[0]);
    int new_fd = as_int(arguments.values[1]);
    auto description = TRY(file(old_fd));
    if (new_fd < 0 || static_cast<size_t>(new_fd) >= max_open_files)
        return Error::from_errno(EBADF);
    if (old_fd == new_fd)
        return static_cast<FlatPtr>(new_fd);
    FileDescriptor replaced;
    {
        SpinLockGuard guard(m_files_lock);
        replaced = move(m_files[new_fd]);
        m_files[new_fd] = { move(description), false };
    }
    return static_cast<FlatPtr>(new_fd);
}

ErrorOr<FlatPtr> Process::sys_pipe(TrapFrame&, SyscallArguments const& arguments)
{
    int flags = as_int(arguments.values[1]);
    auto pipe = TRY(Pipe::create());
    auto reader = TRY(FileDescription::create_for_pipe(pipe, false));
    auto writer = TRY(FileDescription::create_for_pipe(pipe, true));
    if (flags & O_NONBLOCK) {
        reader->set_status_flags(O_NONBLOCK);
        writer->set_status_flags(O_NONBLOCK);
    }
    int fds[2];
    fds[0] = TRY(allocate_fd(reader, flags & O_CLOEXEC));
    auto second = allocate_fd(writer, flags & O_CLOEXEC);
    if (second.is_error()) {
        SpinLockGuard guard(m_files_lock);
        m_files[fds[0]] = {};
        return second.release_error();
    }
    fds[1] = second.value();
    if (auto result = copy_to_user(arguments.values[0], fds); result.is_error()) {
        SpinLockGuard guard(m_files_lock);
        m_files[fds[0]] = {};
        m_files[fds[1]] = {};
        return result.release_error();
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_mkdir(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    TRY(VirtualFileSystem::mkdir(path.view(), m_cwd.view(), static_cast<u32>(arguments.values[1])));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_rmdir(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    TRY(VirtualFileSystem::rmdir(path.view(), m_cwd.view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_unlink(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    TRY(VirtualFileSystem::unlink(path.view(), m_cwd.view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_rename(TrapFrame&, SyscallArguments const& arguments)
{
    auto old_path = TRY(copy_path_from_user(arguments.values[0]));
    auto new_path = TRY(copy_path_from_user(arguments.values[1]));
    TRY(VirtualFileSystem::rename(old_path.view(), new_path.view(), m_cwd.view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getcwd(TrapFrame&, SyscallArguments const& arguments)
{
    size_t size = arguments.values[1];
    if (m_cwd.length() + 1 > size)
        return Error::from_errno(ERANGE);
    TRY(copy_to_user(arguments.values[0], m_cwd.characters(), m_cwd.length() + 1));
    return m_cwd.length();
}

ErrorOr<FlatPtr> Process::sys_chdir(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), m_cwd.view()));
    if (!resolved.inode->is_directory())
        return Error::from_errno(ENOTDIR);
    m_cwd = move(resolved.path);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getdents(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (!is_user_range(arguments.values[1], arguments.values[2]))
        return Error::from_errno(EFAULT);
    return TRY(description->read_directory_entries(IOBuffer::user(arguments.values[1]), arguments.values[2]));
}

ErrorOr<FlatPtr> Process::sys_fsync(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    TRY(description->sync());
    return 0;
}

ErrorOr<FlatPtr> Process::sys_ftruncate(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    i64 size = static_cast<i64>(arguments.values[1]);
    if (size < 0)
        return Error::from_errno(EINVAL);
    TRY(description->truncate(static_cast<u64>(size)));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_readlink(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), m_cwd.view(), false));
    char target[VirtualFileSystem::max_path];
    size_t length = TRY(resolved.inode->read_link({ reinterpret_cast<u8*>(target), sizeof(target) }));
    length = min<size_t>(length, arguments.values[2]);
    TRY(copy_to_user(arguments.values[1], target, length));
    return length;
}

ErrorOr<FlatPtr> Process::sys_symlink(TrapFrame&, SyscallArguments const& arguments)
{
    auto target = TRY(copy_path_from_user(arguments.values[0]));
    auto link_path = TRY(copy_path_from_user(arguments.values[1]));
    TRY(VirtualFileSystem::symlink(target.view(), link_path.view(), m_cwd.view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sync(TrapFrame&, SyscallArguments const&)
{
    TRY(VirtualFileSystem::sync_all());
    return 0;
}

ErrorOr<FlatPtr> Process::sys_fcntl(TrapFrame&, SyscallArguments const& arguments)
{
    int fd = as_int(arguments.values[0]);
    int command = as_int(arguments.values[1]);
    u64 argument = arguments.values[2];
    auto description = TRY(file(fd));
    switch (command) {
    case F_DUPFD:
        if (argument >= max_open_files)
            return Error::from_errno(EINVAL);
        return static_cast<FlatPtr>(TRY(allocate_fd(move(description), false, static_cast<int>(argument))));
    case F_GETFD: {
        SpinLockGuard guard(m_files_lock);
        return m_files[fd].close_on_exec ? FD_CLOEXEC : 0;
    }
    case F_SETFD: {
        SpinLockGuard guard(m_files_lock);
        m_files[fd].close_on_exec = argument & FD_CLOEXEC;
        return 0;
    }
    case F_GETFL:
        return static_cast<FlatPtr>(description->flags());
    case F_SETFL:
        description->set_status_flags(static_cast<int>(argument));
        return 0;
    default:
        return Error::from_errno(EINVAL);
    }
}

ErrorOr<FlatPtr> Process::sys_access(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    int mode = as_int(arguments.values[1]);
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), m_cwd.view()));
    auto metadata = resolved.inode->metadata();
    // No users yet (M6): only the permission bits for "anyone" matter.
    if ((mode & X_OK) && !(metadata.permissions & 0111))
        return Error::from_errno(EACCES);
    if ((mode & W_OK) && resolved.inode->file_system().is_read_only())
        return Error::from_errno(EROFS);
    return 0;
}

}
