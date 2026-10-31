/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/FileSystem.h>
#include <Kernel/FileSystem/Inode.h>

namespace Kernel {

u32 InodeMetadata::mode() const
{
    u32 type_bits = S_IFREG;
    switch (type) {
    case InodeType::File:
        type_bits = S_IFREG;
        break;
    case InodeType::Directory:
        type_bits = S_IFDIR;
        break;
    case InodeType::SymbolicLink:
        type_bits = S_IFLNK;
        break;
    case InodeType::CharacterDevice:
        type_bits = S_IFCHR;
        break;
    case InodeType::BlockDevice:
        type_bits = S_IFBLK;
        break;
    case InodeType::Fifo:
        type_bits = S_IFIFO;
        break;
    }
    return type_bits | (permissions & 07777);
}

vulture_stat InodeMetadata::to_stat(u32 file_system_id) const
{
    vulture_stat result {};
    result.st_dev = file_system_id;
    result.st_ino = index;
    result.st_mode = mode();
    result.st_nlink = link_count;
    result.st_uid = uid;
    result.st_gid = gid;
    result.st_size = static_cast<long long>(size);
    result.st_blksize = block_size;
    result.st_blocks = static_cast<long long>((size + 511) / 512);
    result.st_mtime_seconds = modification_time;
    result.st_rdev = device;
    return result;
}

u8 directory_entry_type(InodeType type)
{
    switch (type) {
    case InodeType::File:
        return DT_REG;
    case InodeType::Directory:
        return DT_DIR;
    case InodeType::SymbolicLink:
        return DT_LNK;
    case InodeType::CharacterDevice:
        return DT_CHR;
    case InodeType::BlockDevice:
        return DT_BLK;
    case InodeType::Fifo:
        return DT_FIFO;
    }
    return DT_UNKNOWN;
}

ErrorOr<size_t> Inode::read(u64, size_t, IOBuffer const&)
{
    return Error::from_errno(is_directory() ? EISDIR : EINVAL);
}

ErrorOr<size_t> Inode::write(u64, size_t, IOBuffer const&)
{
    if (file_system().is_read_only())
        return Error::from_errno(EROFS);
    return Error::from_errno(is_directory() ? EISDIR : EINVAL);
}

ErrorOr<void> Inode::truncate(u64)
{
    if (file_system().is_read_only())
        return Error::from_errno(EROFS);
    return Error::from_errno(is_directory() ? EISDIR : EINVAL);
}

ErrorOr<NonnullRefPtr<Inode>> Inode::lookup(StringView)
{
    return Error::from_errno(ENOTDIR);
}

ErrorOr<void> Inode::traverse(DirectoryVisitor&)
{
    return Error::from_errno(ENOTDIR);
}

ErrorOr<NonnullRefPtr<Inode>> Inode::create_child(StringView, InodeType, u32, StringView)
{
    if (!is_directory())
        return Error::from_errno(ENOTDIR);
    return Error::from_errno(file_system().is_read_only() ? EROFS : ENOTSUP);
}

ErrorOr<void> Inode::remove_child(StringView)
{
    if (!is_directory())
        return Error::from_errno(ENOTDIR);
    return Error::from_errno(file_system().is_read_only() ? EROFS : ENOTSUP);
}

ErrorOr<void> Inode::add_child(StringView, Inode&)
{
    if (!is_directory())
        return Error::from_errno(ENOTDIR);
    return Error::from_errno(file_system().is_read_only() ? EROFS : ENOTSUP);
}

ErrorOr<size_t> Inode::read_link(Bytes)
{
    return Error::from_errno(EINVAL);
}

ErrorOr<void> Inode::set_permissions(u32)
{
    return Error::from_errno(file_system().is_read_only() ? EROFS : ENOTSUP);
}

ErrorOr<void> Inode::sync()
{
    return {};
}

ErrorOr<bool> Inode::is_empty_directory()
{
    struct Counter final : DirectoryVisitor {
        bool empty { true };
        VisitDecision visit(StringView name, InodeIndex, InodeType) override
        {
            if (name == "."_sv || name == ".."_sv)
                return VisitDecision::Continue;
            empty = false;
            return VisitDecision::Stop;
        }
    } counter;
    TRY(traverse(counter));
    return counter.empty;
}

ErrorOr<PhysicalAddress> Inode::mapped_page(u64)
{
    return Error::from_errno(ENODEV);
}

}
