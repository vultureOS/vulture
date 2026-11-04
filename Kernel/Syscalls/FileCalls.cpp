/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Security/Seal.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/socket.h>
#include <Kernel/API/POSIX/termios.h>
#include <Kernel/API/POSIX/time.h>
#include <Kernel/FileSystem/FSEvents.h>
#include <Kernel/FileSystem/Pipe.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/TTY/TTY.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Time/Time.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel {

static int as_int(u64 value)
{
    return static_cast<int>(static_cast<i32>(value));
}

// The directory that a path given with this descriptor starts from. A path that's absolute doesn't care.
ErrorOr<String> Process::directory_for(int fd, StringView path)
{
    if (fd == AT_FDCWD || path.starts_with("/"_sv))
        return current_directory();
    auto description = TRY(file(fd));
    if (!description->inode() || !description->inode()->is_directory() || description->path().is_empty())
        return Error::from_errno(ENOTDIR);
    return String::from_view(description->path());
}

ErrorOr<FlatPtr> Process::open_from(StringView path, StringView directory, int flags, u32 mode)
{
    auto description = TRY(VirtualFileSystem::open(path, directory, flags, mode & ~m_umask));
    // A session leader without a terminal gets the first one it opens.
    if (auto* tty = description->inode() ? description->inode()->as_tty() : nullptr; tty && !(flags & O_NOCTTY) && sid() == m_pid && !m_controlling_terminal)
        (void)set_controlling_terminal(*tty);
    return static_cast<FlatPtr>(TRY(allocate_fd(move(description), flags & O_CLOEXEC)));
}

ErrorOr<FlatPtr> Process::sys_open(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    return open_from(path.view(), current_directory().view(), as_int(arguments.values[1]), static_cast<u32>(arguments.values[2]));
}

ErrorOr<FlatPtr> Process::sys_openat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    return open_from(path.view(), directory.view(), as_int(arguments.values[2]), static_cast<u32>(arguments.values[3]));
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
    if (request == FIONREAD && (description->is_pipe() || description->local_socket() || description->socket())) {
        TRY(copy_to_user(arguments.values[2], static_cast<int>(description->bytes_to_read())));
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
    FileDescriptor closed = take_file_descriptor(as_int(arguments.values[0]));
    if (!closed.description)
        return Error::from_errno(EBADF);
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

// The same, and not cancellation points: for what the C library does itself, with its locks held (Mac has these too).
ErrorOr<FlatPtr> Process::sys_read_nocancel(TrapFrame& frame, SyscallArguments const& arguments)
{
    return sys_read(frame, arguments);
}

ErrorOr<FlatPtr> Process::sys_write_nocancel(TrapFrame& frame, SyscallArguments const& arguments)
{
    return sys_write(frame, arguments);
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

ErrorOr<FlatPtr> Process::sys_fstatat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    return stat_path(path.view(), directory.view(), arguments.values[2], !(arguments.values[3] & AT_SYMLINK_NOFOLLOW));
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
    if (new_fd < 0 || static_cast<u64>(new_fd) >= min<u64>(m_limits[VULTURE_RLIMIT_NOFILE].rlim_cur, max_open_files))
        return Error::from_errno(EBADF);
    if (old_fd == new_fd)
        return static_cast<FlatPtr>(new_fd);
    TRY(ensure_file_capacity(static_cast<size_t>(new_fd) + 1));
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
        release_fd(fds[0]);
        return second.release_error();
    }
    fds[1] = second.value();
    if (auto result = copy_to_user(arguments.values[0], fds); result.is_error()) {
        release_fd(fds[0]);
        release_fd(fds[1]);
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

ErrorOr<FlatPtr> Process::sys_mkdirat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    TRY(VirtualFileSystem::mkdir(path.view(), directory.view(), static_cast<u32>(arguments.values[2]) & ~m_umask));
    return 0;
}

// A named pipe, or nothing much else: device files aren't for making.
ErrorOr<FlatPtr> Process::sys_mknod(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    auto const mode = static_cast<u32>(arguments.values[1]);
    auto const directory = current_directory();
    u32 const permissions = mode & ~m_umask & 07777;
    switch (mode & S_IFMT) {
    case S_IFIFO:
        TRY(VirtualFileSystem::make_node(path.view(), directory.view(), InodeType::Fifo, permissions));
        return 0;
    case S_IFREG:
    case 0: {
        (void)TRY(VirtualFileSystem::open(path.view(), directory.view(), O_CREAT | O_EXCL | O_WRONLY, permissions));
        return 0;
    }
    default:
        return Error::from_errno(m_credentials.is_superuser() ? ENOTSUP : EPERM);
    }
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

ErrorOr<FlatPtr> Process::sys_unlinkat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    auto const flags = as_int(arguments.values[2]);
    if (flags & ~AT_REMOVEDIR)
        return Error::from_errno(EINVAL);
    if (flags & AT_REMOVEDIR)
        TRY(VirtualFileSystem::rmdir(path.view(), directory.view()));
    else
        TRY(VirtualFileSystem::unlink(path.view(), directory.view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_rename(TrapFrame&, SyscallArguments const& arguments)
{
    auto old_path = TRY(copy_path_from_user(arguments.values[0]));
    auto new_path = TRY(copy_path_from_user(arguments.values[1]));
    TRY(VirtualFileSystem::rename(old_path.view(), new_path.view(), current_directory().view()));
    return 0;
}

// Both paths absolute by the time they get to the filesystem, as they may be from different places.
ErrorOr<String> Process::absolute_path_for(int fd, StringView path)
{
    if (path.starts_with("/"_sv))
        return String::from_view(path);
    if (path.is_empty())
        return Error::from_errno(ENOENT);
    auto directory = TRY(directory_for(fd, path));
    StringBuilder builder;
    if (directory.view() != "/"_sv)
        builder.append(directory.view());
    builder.append('/');
    builder.append(path);
    return builder.to_string();
}

ErrorOr<FlatPtr> Process::sys_renameat(TrapFrame&, SyscallArguments const& arguments)
{
    auto old_path = TRY(copy_path_from_user(arguments.values[1]));
    auto new_path = TRY(copy_path_from_user(arguments.values[3]));
    auto old_absolute = TRY(absolute_path_for(as_int(arguments.values[0]), old_path.view()));
    auto new_absolute = TRY(absolute_path_for(as_int(arguments.values[2]), new_path.view()));
    TRY(VirtualFileSystem::rename(old_absolute.view(), new_absolute.view(), "/"_sv));
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

ErrorOr<FlatPtr> Process::sys_fchdir(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    auto* inode = description->inode();
    if (!inode || !inode->is_directory())
        return Error::from_errno(ENOTDIR);
    TRY(check_access(inode->metadata(), X_OK));
    auto path = TRY(String::from_view(description->path()));
    if (path.is_empty())
        return Error::from_errno(ENOTDIR);
    MutexLocker locker(m_cwd_lock);
    m_cwd = move(path);
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
    if (description->inode())
        TRY(description->inode()->check_flags_allow_change());
    TRY(description->truncate(static_cast<u64>(size)));
    if (description->inode() && !description->path().is_empty())
        FSEvents::record(*description->inode(), description->path(), FSE_ITEM_MODIFIED);
    return 0;
}

static ErrorOr<FlatPtr> read_link_at(StringView path, StringView directory, FlatPtr buffer, size_t size)
{
    auto resolved = TRY(VirtualFileSystem::resolve(path, directory, false));
    TRY(sandbox_check(SandboxOperation::FileMetadata, resolved.path.view()));
    char target[VirtualFileSystem::max_path];
    size_t length = TRY(resolved.inode->read_link({ reinterpret_cast<u8*>(target), sizeof(target) }));
    length = min<size_t>(length, size);
    TRY(copy_to_user(buffer, target, length));
    return length;
}

ErrorOr<FlatPtr> Process::sys_readlink(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    return read_link_at(path.view(), current_directory().view(), arguments.values[1], arguments.values[2]);
}

ErrorOr<FlatPtr> Process::sys_readlinkat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    return read_link_at(path.view(), directory.view(), arguments.values[2], arguments.values[3]);
}

ErrorOr<FlatPtr> Process::sys_symlink(TrapFrame&, SyscallArguments const& arguments)
{
    auto target = TRY(copy_path_from_user(arguments.values[0]));
    auto link_path = TRY(copy_path_from_user(arguments.values[1]));
    TRY(VirtualFileSystem::symlink(target.view(), link_path.view(), current_directory().view()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_symlinkat(TrapFrame&, SyscallArguments const& arguments)
{
    auto target = TRY(copy_path_from_user(arguments.values[0]));
    auto link_path = TRY(copy_path_from_user(arguments.values[2]));
    auto directory = TRY(directory_for(as_int(arguments.values[1]), link_path.view()));
    TRY(VirtualFileSystem::symlink(target.view(), link_path.view(), directory.view()));
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
        if (argument >= min<u64>(m_limits[VULTURE_RLIMIT_NOFILE].rlim_cur, max_open_files))
            return Error::from_errno(EINVAL);
        return static_cast<FlatPtr>(TRY(allocate_fd(move(description), false, static_cast<int>(argument))));
    case F_GETFD: {
        SpinLockGuard guard(m_files_lock);
        auto* entry = file_entry(fd);
        if (!entry)
            return Error::from_errno(EBADF);
        return entry->close_on_exec ? FD_CLOEXEC : 0;
    }
    case F_SETFD: {
        SpinLockGuard guard(m_files_lock);
        auto* entry = file_entry(fd);
        if (!entry)
            return Error::from_errno(EBADF);
        entry->close_on_exec = argument & FD_CLOEXEC;
        return 0;
    }
    case F_DUPFD_CLOEXEC:
        if (argument >= min<u64>(m_limits[VULTURE_RLIMIT_NOFILE].rlim_cur, max_open_files))
            return Error::from_errno(EINVAL);
        return static_cast<FlatPtr>(TRY(allocate_fd(move(description), true, static_cast<int>(argument))));
    case F_GETPATH: {
        // Into a buffer of MAXPATHLEN, as a Mac's does: there's no size to go by.
        auto path = description->path();
        if (path.is_empty() || path.length() + 1 > VirtualFileSystem::max_path)
            return Error::from_errno(EBADF);
        TRY(copy_to_user(argument, path.characters_without_null_termination(), path.length()));
        TRY(copy_to_user(argument + path.length(), '\0'));
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

static ErrorOr<FlatPtr> check_path_access(StringView path, StringView directory, int mode, bool follow)
{
    auto resolved = TRY(VirtualFileSystem::resolve(path, directory, follow));
    TRY(sandbox_check(SandboxOperation::FileMetadata, resolved.path.view()));
    auto metadata = resolved.inode->metadata();
    TRY(check_access(metadata, mode & (R_OK | W_OK | X_OK)));
    if ((mode & W_OK) && resolved.inode->file_system().is_read_only())
        return Error::from_errno(EROFS);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_access(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    return check_path_access(path.view(), current_directory().view(), as_int(arguments.values[1]), true);
}

ErrorOr<FlatPtr> Process::sys_faccessat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    return check_path_access(path.view(), directory.view(), as_int(arguments.values[2]), !(arguments.values[3] & AT_SYMLINK_NOFOLLOW));
}

// --- Several buffers at once ---

// A pipe's or a socket's writes have to go in one piece (a datagram stays one, and a pipe's short writes don't get mixed up with
// anyone else's), so those are gathered first, as far as what a single write of them could be anyway.
static constexpr size_t max_gathered = 256 * KiB;

ErrorOr<FlatPtr> Process::sys_readv(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    int const count = as_int(arguments.values[2]);
    if (count <= 0 || count > VULTURE_IOV_MAX)
        return Error::from_errno(EINVAL);
    Vector<vulture_iovec> vectors;
    TRY(vectors.try_resize(static_cast<size_t>(count)));
    TRY(copy_from_user(vectors.data(), arguments.values[1], static_cast<size_t>(count) * sizeof(vulture_iovec)));
    u64 total = 0;
    for (auto const& vector : vectors) {
        if (__builtin_add_overflow(total, vector.iov_len, &total) || total > 0x7fffffffull)
            return Error::from_errno(EINVAL);
        if (!is_user_range(reinterpret_cast<FlatPtr>(vector.iov_base), vector.iov_len))
            return Error::from_errno(EFAULT);
    }
    size_t done = 0;
    for (auto const& vector : vectors) {
        if (vector.iov_len == 0)
            continue;
        auto read = description->read(IOBuffer::user(reinterpret_cast<FlatPtr>(vector.iov_base)), vector.iov_len);
        if (read.is_error()) {
            if (done)
                return done;
            return read.release_error();
        }
        done += read.value();
        if (read.value() < vector.iov_len)
            break;
    }
    return done;
}

ErrorOr<FlatPtr> Process::sys_writev(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    int const count = as_int(arguments.values[2]);
    if (count <= 0 || count > VULTURE_IOV_MAX)
        return Error::from_errno(EINVAL);
    Vector<vulture_iovec> vectors;
    TRY(vectors.try_resize(static_cast<size_t>(count)));
    TRY(copy_from_user(vectors.data(), arguments.values[1], static_cast<size_t>(count) * sizeof(vulture_iovec)));
    u64 total = 0;
    for (auto const& vector : vectors) {
        if (__builtin_add_overflow(total, vector.iov_len, &total) || total > 0x7fffffffull)
            return Error::from_errno(EINVAL);
        if (!is_user_range(reinterpret_cast<FlatPtr>(vector.iov_base), vector.iov_len))
            return Error::from_errno(EFAULT);
    }
    if (total == 0)
        return 0;
    bool const in_one_piece = description->is_pipe() || description->socket() || description->local_socket();
    if (in_one_piece && total <= max_gathered) {
        Vector<u8> gathered;
        TRY(gathered.try_resize(total));
        size_t at = 0;
        for (auto const& vector : vectors) {
            if (vector.iov_len)
                TRY(copy_from_user(gathered.data() + at, reinterpret_cast<FlatPtr>(vector.iov_base), vector.iov_len));
            at += vector.iov_len;
        }
        return TRY(description->write(IOBuffer::kernel(gathered.data()), total));
    }
    size_t done = 0;
    for (auto const& vector : vectors) {
        if (vector.iov_len == 0)
            continue;
        auto written = description->write(IOBuffer::user(reinterpret_cast<FlatPtr>(vector.iov_base)), vector.iov_len);
        if (written.is_error()) {
            if (done)
                return done;
            return written.release_error();
        }
        done += written.value();
        if (written.value() < vector.iov_len)
            break;
    }
    return done;
}

// --- Limits ---

ErrorOr<FlatPtr> Process::sys_getrlimit(TrapFrame&, SyscallArguments const& arguments)
{
    int const resource = as_int(arguments.values[0]);
    if (resource < 0 || resource >= VULTURE_RLIM_NLIMITS)
        return Error::from_errno(EINVAL);
    vulture_rlimit limit;
    {
        SpinLockGuard guard(m_files_lock);
        limit = m_limits[resource];
    }
    TRY(copy_to_user(arguments.values[1], limit));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_setrlimit(TrapFrame&, SyscallArguments const& arguments)
{
    int const resource = as_int(arguments.values[0]);
    if (resource < 0 || resource >= VULTURE_RLIM_NLIMITS)
        return Error::from_errno(EINVAL);
    auto const wanted = TRY(copy_from_user<vulture_rlimit>(arguments.values[1]));
    if (wanted.rlim_cur > wanted.rlim_max)
        return Error::from_errno(EINVAL);
    SpinLockGuard guard(m_files_lock);
    if (wanted.rlim_max > m_limits[resource].rlim_max && !m_credentials.is_superuser())
        return Error::from_errno(EPERM);
    m_limits[resource] = wanted;
    return 0;
}

// --- Ownership, permissions, links and times ---

static ErrorOr<void> change_permissions(Inode& inode, u32 mode)
{
    TRY(inode.check_flags_allow_change());
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
    TRY(inode.check_flags_allow_change());
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
    FSEvents::record(*resolved.inode, resolved.path.view(), FSE_ITEM_METADATA_CHANGED);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_fchmodat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), directory.view(), !(arguments.values[3] & AT_SYMLINK_NOFOLLOW)));
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(change_permissions(*resolved.inode, static_cast<u32>(arguments.values[2])));
    FSEvents::record(*resolved.inode, resolved.path.view(), FSE_ITEM_METADATA_CHANGED);
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
    if (!description->path().is_empty())
        FSEvents::record(*description->inode(), description->path(), FSE_ITEM_METADATA_CHANGED);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_chown(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[0]));
    bool follow = !(arguments.values[3] & AT_SYMLINK_NOFOLLOW);
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), current_directory().view(), follow));
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(change_owner(*resolved.inode, static_cast<i32>(arguments.values[1]), static_cast<i32>(arguments.values[2])));
    FSEvents::record(*resolved.inode, resolved.path.view(), FSE_ITEM_OWNER_CHANGED);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_fchownat(TrapFrame&, SyscallArguments const& arguments)
{
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), directory.view(), !(arguments.values[4] & AT_SYMLINK_NOFOLLOW)));
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(change_owner(*resolved.inode, static_cast<i32>(arguments.values[2]), static_cast<i32>(arguments.values[3])));
    FSEvents::record(*resolved.inode, resolved.path.view(), FSE_ITEM_OWNER_CHANGED);
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

ErrorOr<FlatPtr> Process::sys_linkat(TrapFrame&, SyscallArguments const& arguments)
{
    auto existing = TRY(copy_path_from_user(arguments.values[1]));
    auto created = TRY(copy_path_from_user(arguments.values[3]));
    auto existing_absolute = TRY(absolute_path_for(as_int(arguments.values[0]), existing.view()));
    auto created_absolute = TRY(absolute_path_for(as_int(arguments.values[2]), created.view()));
    // AT_SYMLINK_FOLLOW: a link to what a link names, not to the link. Which is what its target would have been.
    if (arguments.values[4] & AT_SYMLINK_FOLLOW) {
        auto resolved = TRY(VirtualFileSystem::resolve(existing_absolute.view(), "/"_sv));
        existing_absolute = move(resolved.path);
    }
    TRY(VirtualFileSystem::link(existing_absolute.view(), created_absolute.view(), "/"_sv));
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
    auto path = TRY(copy_path_from_user(arguments.values[1]));
    auto directory = TRY(directory_for(as_int(arguments.values[0]), path.view()));
    bool follow = !(arguments.values[3] & AT_SYMLINK_NOFOLLOW);
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), directory.view(), follow));
    TRY(sandbox_check(SandboxOperation::FileWrite, resolved.path.view()));
    TRY(change_times(*resolved.inode, arguments.values[2]));
    FSEvents::record(*resolved.inode, resolved.path.view(), FSE_ITEM_METADATA_CHANGED);
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
    if (!description->path().is_empty())
        FSEvents::record(*description->inode(), description->path(), FSE_ITEM_METADATA_CHANGED);
    return 0;
}

}
