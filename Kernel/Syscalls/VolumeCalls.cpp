/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/FileSystem/FATFS.h>
#include <Kernel/FileSystem/FSEvents.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/TmpFS.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Security/Sandbox.h>
#include <Kernel/Security/Seal.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

// Volumes, for root. What it can mount is what it can read: a temporary one, or a FAT disk. M20 brings the rest, and people plugging
// things in.
ErrorOr<FlatPtr> Process::sys_mount(TrapFrame&, SyscallArguments const& arguments)
{
    if (!m_credentials.is_superuser())
        return Error::from_errno(EPERM);
    auto type = TRY(copy_path_from_user(arguments.values[0]));
    auto directory = TRY(copy_path_from_user(arguments.values[1]));
    auto target = TRY(VirtualFileSystem::resolve(directory.view(), current_directory().view()));
    if (!target.inode->is_directory())
        return Error::from_errno(ENOTDIR);

    if (type.view() == "tmpfs"_sv) {
        TRY(VirtualFileSystem::mount(TRY(TmpFS::create()), target.path.view()));
        return 0;
    }
    if (type.view() == "msdos"_sv) {
        if (!arguments.values[3])
            return Error::from_errno(EINVAL);
        auto device_path = TRY(copy_path_from_user(arguments.values[3]));
        auto device_node = TRY(VirtualFileSystem::resolve(device_path.view(), current_directory().view()));
        if (device_node.inode->metadata().type != InodeType::BlockDevice)
            return Error::from_errno(ENOTBLK);
        auto const name = device_node.inode->block_device_name();
        for (auto& device : BlockDevice::all()) {
            if (device->name() == name) {
                TRY(VirtualFileSystem::mount(TRY(create_fat_file_system(*device)), target.path.view()));
                return 0;
            }
        }
        return Error::from_errno(ENXIO);
    }
    return Error::from_errno(EOPNOTSUPP);
}

ErrorOr<FlatPtr> Process::sys_unmount(TrapFrame&, SyscallArguments const& arguments)
{
    if (!m_credentials.is_superuser())
        return Error::from_errno(EPERM);
    auto directory = TRY(copy_path_from_user(arguments.values[0]));
    TRY(VirtualFileSystem::unmount(directory.view(), current_directory().view(), [](FileSystem& file_system, StringView path) {
        for (auto& process : Process::all()) {
            if (process->is_using(file_system, path))
                return true;
        }
        return false;
    }));
    return 0;
}

// What a call about a file is about: an open one, or a path.
struct Target {
    NonnullRefPtr<Inode> inode;
    String path;
};

static ErrorOr<Target> target_for(Process& process, int fd, FlatPtr path_address, bool follow)
{
    if (fd >= 0) {
        auto description = TRY(process.file(fd));
        if (!description->inode())
            return Error::from_errno(EBADF);
        return Target { *description->inode(), TRY(String::from_view(description->path())) };
    }
    auto path = TRY(process.copy_path_from_user(path_address));
    auto resolved = TRY(VirtualFileSystem::resolve(path.view(), process.current_directory().view(), follow));
    return Target { resolved.inode, move(resolved.path) };
}

static constexpr int read_access = 4;
static constexpr int write_access = 2;

// The largest extended attribute that can be asked for in one go, as a Mac has it.
static constexpr size_t largest_xattr = 64 * MiB;

// xattr(operation | options << 8, fd or -1, path, name, buffer, size)
ErrorOr<FlatPtr> Process::sys_xattr(TrapFrame&, SyscallArguments const& arguments)
{
    int const operation = static_cast<int>(arguments.values[0] & 0xff);
    int const options = static_cast<int>(arguments.values[0] >> 8);
    auto const target = TRY(target_for(*this, static_cast<int>(arguments.values[1]), arguments.values[2], !(options & XATTR_NOFOLLOW)));
    auto& inode = *target.inode;
    FlatPtr const buffer = arguments.values[4];
    size_t const size = arguments.values[5];
    if (size > largest_xattr)
        return Error::from_errno(E2BIG);
    if (size && !is_user_range(buffer, size))
        return Error::from_errno(EFAULT);

    auto copy_to = [&](u8 const* bytes, size_t length) -> ErrorOr<FlatPtr> {
        if (buffer && size) {
            if (length > size)
                return Error::from_errno(ERANGE);
            TRY(copy_to_user(buffer, bytes, length));
        }
        return length;
    };

    if (operation == VULTURE_XATTR_LIST) {
        if (!target.path.is_empty())
            TRY(sandbox_check(SandboxOperation::FileMetadata, target.path.view()));
        TRY(check_access(inode.metadata(), read_access));
        auto const names = TRY(inode.list_xattrs());
        size_t total = 0;
        for (auto const& name : names)
            total += name.view().length() + 1;
        if (!buffer || !size || total == 0)
            return total;
        if (total > size)
            return Error::from_errno(ERANGE);
        Vector<u8> joined;
        TRY(joined.try_resize(total));
        size_t at = 0;
        for (auto const& name : names) {
            __builtin_memcpy(joined.begin() + at, name.view().characters_without_null_termination(), name.view().length());
            at += name.view().length();
            joined[at++] = 0;
        }
        return copy_to(joined.begin(), total);
    }

    char name_buffer[XATTR_MAXNAMELEN + 2];
    size_t const name_length = TRY(copy_string_from_user(arguments.values[3], name_buffer, sizeof(name_buffer)));
    if (name_length > XATTR_MAXNAMELEN)
        return Error::from_errno(ENAMETOOLONG);
    StringView const name { name_buffer, name_length };
    if (name.is_empty())
        return Error::from_errno(EINVAL);

    if (operation == VULTURE_XATTR_GET) {
        if (!target.path.is_empty())
            TRY(sandbox_check(SandboxOperation::FileRead, target.path.view()));
        TRY(check_access(inode.metadata(), read_access));
        size_t const needed = TRY(inode.get_xattr(name, {}));
        if (!buffer || !size)
            return needed;
        if (needed > size)
            return Error::from_errno(ERANGE);
        if (needed == 0)
            return 0;
        auto* bytes = static_cast<u8*>(kmalloc(needed));
        if (!bytes)
            return Error::from_errno(ENOMEM);
        auto const got = inode.get_xattr(name, { bytes, needed });
        ErrorOr<FlatPtr> result = got.is_error() ? ErrorOr<FlatPtr>(Error::from_errno(got.error().code())) : copy_to(bytes, got.value());
        kfree(bytes);
        return result;
    }

    if (operation != VULTURE_XATTR_SET && operation != VULTURE_XATTR_REMOVE)
        return Error::from_errno(EINVAL);
    if (!target.path.is_empty())
        TRY(sandbox_check(SandboxOperation::FileWrite, target.path.view()));
    TRY(Seal::check_may_change(inode));
    if (inode.file_system().is_read_only())
        return Error::from_errno(EROFS);
    TRY(check_access(inode.metadata(), write_access));
    TRY(inode.check_flags_allow_change());
    if (operation == VULTURE_XATTR_REMOVE) {
        TRY(inode.remove_xattr(name));
    } else {
        auto* bytes = static_cast<u8*>(kmalloc(max<size_t>(size, 1)));
        if (!bytes)
            return Error::from_errno(ENOMEM);
        ErrorOr<void> result = {};
        if (size)
            result = copy_from_user(bytes, buffer, size);
        if (!result.is_error())
            result = inode.set_xattr(name, { bytes, size }, options);
        kfree(bytes);
        TRY(result);
    }
    if (!target.path.is_empty())
        FSEvents::record(inode, target.path.view(), FSE_ITEM_XATTR_CHANGED);
    return 0;
}

// acl(operation | options << 8, fd or -1, path, entries, how many there's room for)
ErrorOr<FlatPtr> Process::sys_acl(TrapFrame&, SyscallArguments const& arguments)
{
    int const operation = static_cast<int>(arguments.values[0] & 0xff);
    int const options = static_cast<int>(arguments.values[0] >> 8);
    auto const target = TRY(target_for(*this, static_cast<int>(arguments.values[1]), arguments.values[2], !(options & XATTR_NOFOLLOW)));
    auto& inode = *target.inode;
    FlatPtr const buffer = arguments.values[3];
    size_t const count = arguments.values[4];
    if (count > VULTURE_ACL_MAX_ENTRIES)
        return Error::from_errno(EINVAL);
    if (operation == VULTURE_ACL_GET) {
        auto const entries = TRY(inode.acl());
        if (buffer && count) {
            if (entries.size() > count)
                return Error::from_errno(ERANGE);
            TRY(copy_to_user(buffer, entries.data(), entries.size() * sizeof(vulture_acl_entry)));
        }
        return entries.size();
    }
    if (operation != VULTURE_ACL_SET)
        return Error::from_errno(EINVAL);
    if (!target.path.is_empty())
        TRY(sandbox_check(SandboxOperation::FileWrite, target.path.view()));
    TRY(Seal::check_may_change(inode));
    // Its owner's business, like the mode.
    if (!m_credentials.is_superuser() && m_credentials.euid != inode.metadata().uid)
        return Error::from_errno(EPERM);
    Vector<vulture_acl_entry> entries;
    TRY(entries.try_resize(count));
    if (count)
        TRY(copy_from_user(entries.data(), buffer, count * sizeof(vulture_acl_entry)));
    for (auto const& entry : entries) {
        if (entry.kind > 1 || entry.tag > 2)
            return Error::from_errno(EINVAL);
    }
    TRY(inode.set_acl(entries.span()));
    if (!target.path.is_empty())
        FSEvents::record(inode, target.path.view(), FSE_ITEM_METADATA_CHANGED);
    return 0;
}

// clonefile(directory or fd of the source, its path or nothing, directory of the new one, its path, flags)
ErrorOr<FlatPtr> Process::sys_clonefile(TrapFrame&, SyscallArguments const& arguments)
{
    int const source_directory = static_cast<int>(arguments.values[0]);
    int const flags = static_cast<int>(arguments.values[4]);
    auto const new_path = TRY(copy_path_from_user(arguments.values[3]));
    auto const new_directory = TRY(directory_for(static_cast<int>(arguments.values[2]), new_path.view()));
    RefPtr<Inode> source;
    String source_path;
    if (!arguments.values[1]) {
        auto const target = TRY(target_for(*this, source_directory, 0, true));
        source = target.inode;
        source_path = target.path;
    } else {
        auto const path = TRY(copy_path_from_user(arguments.values[1]));
        auto const directory = TRY(directory_for(source_directory, path.view()));
        auto resolved = TRY(VirtualFileSystem::resolve(path.view(), directory.view(), !(flags & CLONE_NOFOLLOW)));
        source = resolved.inode;
        source_path = move(resolved.path);
    }
    // Its owner, for root unless it's asked not to be; otherwise whoever's asking, as it would be for anything they made.
    auto const metadata = source->metadata();
    if (metadata.type != InodeType::File && metadata.type != InodeType::Directory && metadata.type != InodeType::SymbolicLink)
        return Error::from_errno(EINVAL);
    bool const keeps_owner = m_credentials.is_superuser() && !(flags & CLONE_NOOWNERCOPY);
    u32 const uid = keeps_owner ? metadata.uid : m_credentials.euid;
    u32 const gid = keeps_owner ? metadata.gid : m_credentials.egid;
    (void)TRY(VirtualFileSystem::clone(*source, source_path.view(), new_path.view(), new_directory.view(), uid, gid));
    return 0;
}

// chflags(fd or -1, path, flags)
ErrorOr<FlatPtr> Process::sys_chflags(TrapFrame&, SyscallArguments const& arguments)
{
    auto const target = TRY(target_for(*this, static_cast<int>(arguments.values[0]), arguments.values[1], true));
    auto& inode = *target.inode;
    if (!target.path.is_empty())
        TRY(sandbox_check(SandboxOperation::FileWrite, target.path.view()));
    TRY(Seal::check_may_change(inode));
    auto const flags = static_cast<u32>(arguments.values[2]);
    if (!m_credentials.is_superuser()) {
        if (m_credentials.euid != inode.metadata().uid)
            return Error::from_errno(EPERM);
        // Those for root, nobody else gets to change.
        constexpr u32 for_root = SF_ARCHIVED | SF_IMMUTABLE | SF_APPEND;
        if ((flags & for_root) != (inode.metadata().flags & for_root))
            return Error::from_errno(EPERM);
    }
    TRY(inode.set_flags(flags));
    if (!target.path.is_empty())
        FSEvents::record(inode, target.path.view(), FSE_ITEM_METADATA_CHANGED);
    return 0;
}

// fs_snapshot(operation, fd of something on the volume, name or a buffer for the names, its size, flags)
ErrorOr<FlatPtr> Process::sys_fs_snapshot(TrapFrame&, SyscallArguments const& arguments)
{
    if (!m_credentials.is_superuser())
        return Error::from_errno(EPERM);
    int const operation = static_cast<int>(arguments.values[0]);
    auto const target = TRY(target_for(*this, static_cast<int>(arguments.values[1]), 0, true));
    auto& file_system = target.inode->file_system();
    if (operation == VULTURE_SNAPSHOT_LIST) {
        auto const names = TRY(file_system.snapshots());
        size_t total = 0;
        for (auto const& name : names)
            total += name.view().length() + 1;
        FlatPtr const buffer = arguments.values[2];
        size_t const size = arguments.values[3];
        if (!buffer || !size)
            return total;
        if (total > size)
            return Error::from_errno(ERANGE);
        size_t at = 0;
        char const zero = 0;
        for (auto const& name : names) {
            TRY(copy_to_user(buffer + at, name.view().characters_without_null_termination(), name.view().length()));
            at += name.view().length();
            TRY(copy_to_user(buffer + at, &zero, 1));
            ++at;
        }
        return total;
    }
    char name_buffer[260];
    size_t const length = TRY(copy_string_from_user(arguments.values[2], name_buffer, sizeof(name_buffer)));
    if (length > 255)
        return Error::from_errno(ENAMETOOLONG);
    StringView const name { name_buffer, length };
    switch (operation) {
    case VULTURE_SNAPSHOT_CREATE:
        TRY(file_system.create_snapshot(name));
        return 0;
    case VULTURE_SNAPSHOT_DELETE:
        TRY(file_system.delete_snapshot(name));
        return 0;
    case VULTURE_SNAPSHOT_REVERT:
        TRY(file_system.revert_to_snapshot(name));
        return 0;
    default:
        return Error::from_errno(EINVAL);
    }
}

}
