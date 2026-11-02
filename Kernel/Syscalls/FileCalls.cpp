/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Security/Seal.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/termios.h>
#include <Kernel/API/POSIX/time.h>
#include <Kernel/FileSystem/Pipe.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/TTY/TTY.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Time/Time.h>

namespace Kernel {

static int as_int(u64 value)
{
    return static_cast<int>(static_cast<i32>(value));
}

ErrorOr<FlatPtr> Process::sys_open(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    int flags = as_int(arguments.values[1]);
    auto description = TRY(VirtualFileSystem::open(path.view(), current_directory().view(), flags, static_cast<u32>(arguments.values[2]) & ~m_umask));
    // A session leader without a terminal gets the first one it opens.
    if (auto* tty = description->inode()->as_tty(); tty && !(flags & O_NOCTTY) && sid() == m_pid && !m_controlling_terminal && tty->session() == 0)
        (void)set_controlling_terminal(*tty);
    return static_cast<FlatPtr>(TRY(allocate_fd(move(description), flags & O_CLOEXEC)));
}

ErrorOr<FlatPtr> Process::sys_ioctl(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    auto request = static_cast<unsigned>(arguments.values[1]);
    if (request == FIONBIO) {
        auto non_blocking = TRY(copy_from_user<int>(arguments.values[2]));
        description->set_status_flags(non_blocking ? (description->flags() | O_NONBLOCK) : (description->flags() & ~O_NONBLOCK));
        return 0;
    }
    if (description->socket())
        return socket_ioctl(request, arguments.values[2]);
    auto* inode = description->inode();
    if (!inode)
        return Error::from_errno(ENOTTY);
    return inode->ioctl(*description, request, arguments.values[2]);
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

ErrorOr<FlatPtr> Process::sys_pread(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (static_cast<i64>(arguments.values[3]) < 0)
        return Error::from_errno(EINVAL);
    if (!is_user_range(arguments.values[1], arguments.values[2]))
        return Error::from_errno(EFAULT);
    return TRY(description->read_at(IOBuffer::user(arguments.values[1]), arguments.values[2], arguments.values[3]));
}

ErrorOr<FlatPtr> Process::sys_pwrite(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (static_cast<i64>(arguments.values[3]) < 0)
        return Error::from_errno(EINVAL);
    if (arguments.values[2] == 0)
        return 0;
    if (!is_user_range(arguments.values[1], arguments.values[2]))
        return Error::from_errno(EFAULT);
    return TRY(description->write_at(IOBuffer::user(arguments.values[1]), arguments.values[2], arguments.values[3]));
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
    TRY(sandbox_check(SandboxOperation::FileMetadata, resolved.path.view()));
    TRY(copy_to_user(result, resolved.inode->metadata().to_stat(resolved.inode->file_system().id())));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_stat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    return stat_path(path.view(), current_directory().view(), arguments.values[1], true);
}

ErrorOr<FlatPtr> Process::sys_lstat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    return stat_path(path.view(), current_directory().view(), arguments.values[1], false);
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
    TRY(VirtualFileSystem::mkdir(path.view(), current_directory().view(), static_cast<u32>(arguments.values[1]) & ~m_umask));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_rmdir(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    TRY(VirtualFileSystem::rmdir(path.view(), current_directory().view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_unlink(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    TRY(VirtualFileSystem::unlink(path.view(), current_directory().view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_rename(TrapFrame&, SyscallArguments const& arguments)
{
    auto old_path = TRY(copy_path_from_user(arguments.values[0]));
    auto new_path = TRY(copy_path_from_user(arguments.values[1]));
    TRY(VirtualFileSystem::rename(old_path.view(), new_path.view(), current_directory().view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getcwd(TrapFrame&, SyscallArguments const& arguments)
{
    size_t size = arguments.values[1];
    auto directory = current_directory();
    if (directory.length() + 1 > size)
        return Error::from_errno(ERANGE);
    TRY(copy_to_user(arguments.values[0], directory.characters(), directory.length() + 1));
    return directory.length();
}

ErrorOr<FlatPtr> Process::sys_chdir(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), current_directory().view()));
    TRY(sandbox_check(SandboxOperation::FileMetadata, resolved.path.view()));
    auto metadata = resolved.inode->metadata();
    if (metadata.type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    TRY(check_access(metadata, X_OK));
    {
        MutexLocker locker(m_cwd_lock);
        m_cwd = move(resolved.path);
    }
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
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), current_directory().view(), false));
    TRY(sandbox_check(SandboxOperation::FileMetadata, resolved.path.view()));
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
    TRY(VirtualFileSystem::symlink(target.view(), link_path.view(), current_directory().view()));
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
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), current_directory().view()));
    TRY(sandbox_check(SandboxOperation::FileMetadata, resolved.path.view()));
    auto metadata = resolved.inode->metadata();
    TRY(check_access(metadata, mode & (R_OK | W_OK | X_OK)));
    if ((mode & W_OK) && resolved.inode->file_system().is_read_only())
        return Error::from_errno(EROFS);
    return 0;
}

// --- Ownership, permissions, links and times ---

static ErrorOr<void> change_permissions(Inode& inode, u32 mode)
{
    TRY(Seal::check_may_change(inode));
    auto metadata = inode.metadata();
    TRY(check_owner(metadata));
    // Only a group member (or root) may keep the set-group-ID bit.
    auto const* credentials = current_credentials();
    if (credentials && !credentials->is_superuser() && !credentials->in_group(metadata.gid))
        mode &= ~02000u;
    return inode.set_permissions(mode & 07777);
}

static ErrorOr<void> change_owner(Inode& inode, i64 uid, i64 gid)
{
    TRY(Seal::check_may_change(inode));
    auto metadata = inode.metadata();
    auto const* credentials = current_credentials();
    u32 new_uid = uid < 0 ? metadata.uid : static_cast<u32>(uid);
    u32 new_gid = gid < 0 ? metadata.gid : static_cast<u32>(gid);
    if (credentials && !credentials->is_superuser()) {
        // The owner may only change the group, to one of their own.
        if (new_uid != metadata.uid || credentials->euid != metadata.uid || !credentials->in_group(new_gid))
            return Error::from_errno(EPERM);
    }
    TRY(inode.set_owner(new_uid, new_gid));
    // A changed owner loses set-ID bits (they would grant someone else's rights).
    if ((metadata.permissions & 06000) && (new_uid != metadata.uid || new_gid != metadata.gid))
        (void)inode.set_permissions(metadata.permissions & ~06000u);
    return {};
}

ErrorOr<FlatPtr> Process::sys_chmod(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), current_directory().view()));
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(change_permissions(*resolved.inode, static_cast<u32>(arguments.values[1])));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_fchmod(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (!description->inode())
        return Error::from_errno(EINVAL);
    if (m_sandbox && !description->is_writable())
        return Error::from_errno(EPERM);
    TRY(change_permissions(*description->inode(), static_cast<u32>(arguments.values[1])));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_chown(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    bool follow = !(arguments.values[3] & AT_SYMLINK_NOFOLLOW);
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), current_directory().view(), follow));
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(change_owner(*resolved.inode, static_cast<i32>(arguments.values[1]), static_cast<i32>(arguments.values[2])));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_fchown(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (m_sandbox && !description->is_writable())
        return Error::from_errno(EPERM);
    if (!description->inode())
        return Error::from_errno(EINVAL);
    TRY(change_owner(*description->inode(), static_cast<i32>(arguments.values[1]), static_cast<i32>(arguments.values[2])));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_link(TrapFrame&, SyscallArguments const& arguments)
{
    auto existing = TRY(copy_path_from_user(arguments.values[0]));
    auto created = TRY(copy_path_from_user(arguments.values[1]));
    TRY(VirtualFileSystem::link(existing.view(), created.view(), current_directory().view()));
    return 0;
}

// Two timespecs (access, modification), UTIME_NOW or UTIME_OMIT allowed;
// no array means both now.
static ErrorOr<void> change_times(Inode& inode, FlatPtr times_address)
{
    auto metadata = inode.metadata();
    i64 now = static_cast<i64>(Time::wall_clock_nanoseconds());
    i64 times[2] = { now, now };
    bool explicit_times = false;
    if (times_address) {
        vulture_timespec given[2];
        TRY(copy_from_user(given, times_address, sizeof(given)));
        i64 current[2] = { metadata.access_time_ns ? metadata.access_time_ns : metadata.modification_time * 1'000'000'000,
            metadata.modification_time_ns ? metadata.modification_time_ns : metadata.modification_time * 1'000'000'000 };
        for (int i = 0; i < 2; ++i) {
            if (given[i].tv_nsec == UTIME_OMIT) {
                times[i] = current[i];
            } else if (given[i].tv_nsec != UTIME_NOW) {
                if (given[i].tv_nsec < 0 || given[i].tv_nsec >= 1'000'000'000)
                    return Error::from_errno(EINVAL);
                times[i] = given[i].tv_sec * 1'000'000'000 + given[i].tv_nsec;
                explicit_times = true;
            }
        }
    }
    TRY(Seal::check_may_change(inode));
    // Setting times other than now takes ownership; now takes write access.
    if (explicit_times)
        TRY(check_owner(metadata));
    else if (check_owner(metadata).is_error())
        TRY(check_access(metadata, W_OK));
    return inode.set_times(times[0], times[1]);
}

ErrorOr<FlatPtr> Process::sys_utimensat(TrapFrame&, SyscallArguments const& arguments)
{
    // Paths are relative to the working directory (AT_FDCWD) only.
    if (as_int(arguments.values[0]) != AT_FDCWD)
        return Error::from_errno(ENOTSUP);
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    bool follow = !(arguments.values[3] & AT_SYMLINK_NOFOLLOW);
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), current_directory().view(), follow));
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(change_times(*resolved.inode, arguments.values[2]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_futimens(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (m_sandbox && !description->is_writable())
        return Error::from_errno(EPERM);
    if (!description->inode())
        return Error::from_errno(EINVAL);
    TRY(change_times(*description->inode(), arguments.values[1]));
    return 0;
}

}
