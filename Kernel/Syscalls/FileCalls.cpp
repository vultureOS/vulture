/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

static constexpr size_t max_path = 1024;

ErrorOr<FlatPtr> Process::sys_open(TrapFrame&, SyscallArguments const& arguments)
{
    char path[max_path];
    auto length = TRY(copy_string_from_user(arguments.values[0], path, sizeof(path)));
    int flags = static_cast<int>(arguments.values[1]);
    auto file = TRY(OpenFile::open_path({ path, length }, flags));
    return static_cast<FlatPtr>(TRY(allocate_fd(move(file))));
}

ErrorOr<FlatPtr> Process::sys_close(TrapFrame&, SyscallArguments const& arguments)
{
    int fd = static_cast<int>(arguments.values[0]);
    if (fd < 0 || static_cast<size_t>(fd) >= max_open_files)
        return Error::from_errno(EBADF);
    RefPtr<OpenFile> closed;
    {
        SpinLockGuard guard(m_files_lock);
        if (!m_files[fd])
            return Error::from_errno(EBADF);
        closed = move(m_files[fd]);
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_read(TrapFrame&, SyscallArguments const& arguments)
{
    auto file = TRY(this->file(static_cast<int>(arguments.values[0])));
    return TRY(file->read(arguments.values[1], arguments.values[2]));
}

ErrorOr<FlatPtr> Process::sys_write(TrapFrame&, SyscallArguments const& arguments)
{
    auto file = TRY(this->file(static_cast<int>(arguments.values[0])));
    return TRY(file->write(arguments.values[1], arguments.values[2]));
}

ErrorOr<FlatPtr> Process::sys_lseek(TrapFrame&, SyscallArguments const& arguments)
{
    auto file = TRY(this->file(static_cast<int>(arguments.values[0])));
    auto position = TRY(file->seek(static_cast<i64>(arguments.values[1]), static_cast<int>(arguments.values[2])));
    return static_cast<FlatPtr>(position);
}

ErrorOr<FlatPtr> Process::sys_fstat(TrapFrame&, SyscallArguments const& arguments)
{
    auto file = TRY(this->file(static_cast<int>(arguments.values[0])));
    TRY(copy_to_user(arguments.values[1], file->stat()));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_stat(TrapFrame&, SyscallArguments const& arguments)
{
    char path[max_path];
    auto length = TRY(copy_string_from_user(arguments.values[0], path, sizeof(path)));
    auto node = BootArchive::lookup({ path, length });
    if (!node.has_value())
        return Error::from_errno(ENOENT);
    TRY(copy_to_user(arguments.values[1], OpenFile::stat_for(node.value())));
    return 0;
}

}
