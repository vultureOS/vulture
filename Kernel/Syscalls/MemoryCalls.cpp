/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
    if (!inode || inode->metadata().type != InodeType::File)
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
    TRY(m_address_space->protect(address, length, protection));
    return 0;
}

}
