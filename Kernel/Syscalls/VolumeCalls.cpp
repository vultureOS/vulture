/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/FileSystem/FATFS.h>
#include <Kernel/FileSystem/TmpFS.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/UserAccess.h>
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

}
