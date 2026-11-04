/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Security/CodeSigning.h>
#include <Kernel/FileSystem/TmpFS.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

// A file with no name, on a filesystem nobody can see: memory that's shared by mapping it.
static ErrorOr<NonnullRefPtr<TmpFSInode>> create_shared_memory_file(size_t size)
{
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
    return inode;
}

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
        // Code comes from files that are vouched for, not from memory.
        if ((protection & PROT_EXEC) && CodeSigning::is_enforced())
            return Error::from_errno(EPERM);
        // Both sides of a fork keep it, so it's what shared_memory_create() makes, with no descriptor left behind.
        if (shared) {
            auto inode = TRY(create_shared_memory_file(align_up<size_t>(length, page_size)));
            auto* shared_region = TRY(m_address_space->map_file(address, length, protection, fixed, *inode, 0, true, protection & PROT_WRITE));
            return shared_region->base();
        }
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
    if (protection & PROT_EXEC)
        TRY(CodeSigning::check_mapped_file(*inode));
    auto* region = TRY(m_address_space->map_file(address, length, protection, fixed, *inode, offset, shared, shared && description->is_writable()));
    if (protection & PROT_EXEC)
        region->set_checked_code();
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
    auto inode = TRY(create_shared_memory_file(size));
    auto description = TRY(FileDescription::create(NonnullRefPtr<Inode>(*inode), O_RDWR));
    return static_cast<FlatPtr>(TRY(allocate_fd(move(description), flags & O_CLOEXEC)));
}

ErrorOr<FlatPtr> Process::sys_madvise(TrapFrame&, SyscallArguments const& arguments)
{
    // As a Mac has it: an address that isn't a page's is rounded down, not an error. POSIX allows either.
    FlatPtr const given = arguments.values[0];
    FlatPtr const address = align_down<FlatPtr>(given, page_size);
    size_t const length = arguments.values[1] + (given - address);
    int const advice = static_cast<int>(arguments.values[2]);
    switch (advice) {
    case VULTURE_MADV_NORMAL:
    case VULTURE_MADV_RANDOM:
    case VULTURE_MADV_SEQUENTIAL:
    case VULTURE_MADV_WILLNEED:
        if (length)
            TRY(m_address_space->check_mapped(address, length));
        return 0;
    case VULTURE_MADV_DONTNEED:
    case VULTURE_MADV_FREE:
        if (length == 0)
            return 0;
        TRY(m_address_space->discard(address, length));
        return 0;
    default:
        return Error::from_errno(EINVAL);
    }
}

}
