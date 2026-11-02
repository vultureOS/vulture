/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/FileSystem/TmpFS.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

ErrorOr<FlatPtr> Process::sys_mmap(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr address = arguments.values[0];
    size_t length = arguments.values[1];
    int protection = static_cast<int>(arguments.values[2]);
    int flags = static_cast<int>(arguments.values[3]);
    int fd = static_cast<int>(arguments.values[4]);
    u64 offset = arguments.values[5];
    if (length == 0 || (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
        return Error::from_errno(EINVAL);
    // Memory is writable or executable, never both.
    if ((protection & PROT_WRITE) && (protection & PROT_EXEC))
        return Error::from_errno(EACCES);
    bool shared = flags & MAP_SHARED;
    if (shared == static_cast<bool>(flags & MAP_PRIVATE))
        return Error::from_errno(EINVAL); // Exactly one of the two.
    bool fixed = flags & MAP_FIXED;
    if (fixed && address % page_size != 0)
        return Error::from_errno(EINVAL);

    if (flags & MAP_ANONYMOUS) {
        // Shared anonymous memory needs an object both sides of a fork
        // keep; not yet.
        if (shared)
            return Error::from_errno(ENOTSUP);
        auto* region = TRY(m_address_space->map_anonymous(address, length, protection, (flags & MAP_STACK) ? "stack"_sv : "mmap"_sv, fixed));
        return region->base();
    }

    auto description = TRY(file(fd));
    auto* inode = description->inode();
    if (!inode || (inode->metadata().type != InodeType::File && !inode->can_be_mapped()))
        return Error::from_errno(ENODEV);
    if (!description->is_readable())
        return Error::from_errno(EACCES);
    if (shared && (protection & PROT_WRITE) && !description->is_writable())
        return Error::from_errno(EACCES);
    if (offset % page_size != 0)
        return Error::from_errno(EINVAL);
    auto* region = TRY(m_address_space->map_file(address, length, protection, fixed, *inode, offset, shared, shared && description->is_writable()));
    return region->base();
}

ErrorOr<FlatPtr> Process::sys_munmap(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr address = arguments.values[0];
    size_t length = arguments.values[1];
    if (address % page_size != 0 || length == 0)
        return Error::from_errno(EINVAL);
    TRY(m_address_space->unmap(address, length));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_mprotect(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr address = arguments.values[0];
    size_t length = arguments.values[1];
    int protection = static_cast<int>(arguments.values[2]);
    if (address % page_size != 0 || (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
        return Error::from_errno(EINVAL);
    if ((protection & PROT_WRITE) && (protection & PROT_EXEC))
        return Error::from_errno(EACCES);
    TRY(m_address_space->protect(address, length, protection));
    return 0;
}

// shared_memory_create(size, flags): a file with no name, for mapping into
// several processes: what is mapped MAP_SHARED is the same memory everywhere.
// Sent to another process through a port. Flags: O_CLOEXEC.
ErrorOr<FlatPtr> Process::sys_shared_memory_create(TrapFrame&, SyscallArguments const& arguments)
{
    size_t size = arguments.values[0];
    int flags = static_cast<int>(arguments.values[1]);
    if (size == 0 || size > 1ull << 40)
        return Error::from_errno(EINVAL);
    static RefPtr<TmpFS> s_file_system;
    static SpinLock s_file_system_lock;
    RefPtr<TmpFS> file_system;
    {
        SpinLockGuard guard(s_file_system_lock);
        file_system = s_file_system;
    }
    if (!file_system) {
        auto created = TRY(TmpFS::create());
        SpinLockGuard guard(s_file_system_lock);
        if (!s_file_system)
            s_file_system = created;
        file_system = s_file_system;
    }
    auto inode = TRY(TmpFSInode::create(*file_system, InodeType::File, 0600));
    TRY(inode->truncate(size));
    auto description = TRY(FileDescription::create(NonnullRefPtr<Inode>(*inode), O_RDWR));
    return static_cast<FlatPtr>(TRY(allocate_fd(move(description), flags & O_CLOEXEC)));
}

}
