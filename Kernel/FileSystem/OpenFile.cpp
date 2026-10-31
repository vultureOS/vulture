/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/OpenFile.h>
#include <Kernel/Memory/UserAccess.h>
#include <LibBase/New.h>

namespace Kernel {

OpenFile::OpenFile(Kind kind, int flags)
    : m_kind(kind)
    , m_flags(flags)
    , m_close_on_exec(flags & O_CLOEXEC)
{
}

ErrorOr<NonnullRefPtr<OpenFile>> OpenFile::open_console()
{
    auto* file = new (Base::nothrow) OpenFile(Kind::Console, O_RDWR);
    if (!file)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*file);
}

ErrorOr<NonnullRefPtr<OpenFile>> OpenFile::open_path(StringView path, int flags)
{
    auto node = BootArchive::lookup(path);
    // The boot archive is read-only.
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        return Error::from_errno(node.has_value() || (flags & O_CREAT) ? EROFS : ENOENT);
    if (!node.has_value())
        return Error::from_errno(ENOENT);
    if ((flags & O_DIRECTORY) && !node->is_directory)
        return Error::from_errno(ENOTDIR);
    auto* file = new (Base::nothrow) OpenFile(node->is_directory ? Kind::ArchiveDirectory : Kind::ArchiveFile, flags);
    if (!file)
        return Error::from_errno(ENOMEM);
    file->m_node = node.value();
    return adopt_ref(*file);
}

ErrorOr<size_t> OpenFile::read(FlatPtr user_buffer, size_t size)
{
    if ((m_flags & O_ACCMODE) == O_WRONLY)
        return Error::from_errno(EBADF);
    switch (m_kind) {
    case Kind::Console:
        // No input devices yet (M6): end of file.
        return 0;
    case Kind::ArchiveDirectory:
        return Error::from_errno(EISDIR);
    case Kind::ArchiveFile:
        break;
    }
    u64 offset;
    {
        SpinLockGuard guard(m_lock);
        offset = m_offset;
    }
    if (offset >= m_node.data.size())
        return 0;
    size_t count = min<size_t>(size, m_node.data.size() - offset);
    // No lock across the copy: it may fault in pages and sleep.
    TRY(copy_to_user(user_buffer, m_node.data.data() + offset, count));
    SpinLockGuard guard(m_lock);
    m_offset = offset + count;
    return count;
}

ErrorOr<size_t> OpenFile::write(FlatPtr user_buffer, size_t size)
{
    if ((m_flags & O_ACCMODE) == O_RDONLY || m_kind != Kind::Console)
        return Error::from_errno(EBADF);
    // In chunks through a kernel buffer; each chunk is checked before any
    // of it is written.
    char chunk[256];
    size_t written = 0;
    while (written < size) {
        size_t count = min(size - written, sizeof(chunk));
        auto result = copy_from_user(chunk, user_buffer + written, count);
        if (result.is_error()) {
            if (written > 0)
                return written;
            return result.release_error();
        }
        kernel_log().write_raw({ chunk, count });
        written += count;
    }
    return written;
}

ErrorOr<i64> OpenFile::seek(i64 offset, int origin)
{
    if (m_kind == Kind::Console)
        return Error::from_errno(ESPIPE);
    SpinLockGuard guard(m_lock);
    i64 base = 0;
    switch (origin) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = static_cast<i64>(m_offset);
        break;
    case SEEK_END:
        base = static_cast<i64>(m_node.data.size());
        break;
    default:
        return Error::from_errno(EINVAL);
    }
    i64 position;
    if (__builtin_add_overflow(base, offset, &position) || position < 0)
        return Error::from_errno(EINVAL);
    m_offset = static_cast<u64>(position);
    return position;
}

vulture_stat OpenFile::stat_for(BootArchive::Node const& node)
{
    vulture_stat result {};
    result.st_dev = 1;
    result.st_ino = node.inode;
    result.st_mode = (node.is_directory ? S_IFDIR : S_IFREG) | node.mode;
    result.st_nlink = 1;
    result.st_size = static_cast<long long>(node.data.size());
    result.st_blksize = 4096;
    result.st_blocks = static_cast<long long>((node.data.size() + 511) / 512);
    return result;
}

vulture_stat OpenFile::stat() const
{
    if (m_kind == Kind::Console) {
        vulture_stat result {};
        result.st_dev = 2;
        result.st_ino = 1;
        result.st_mode = S_IFCHR | 0620;
        result.st_nlink = 1;
        result.st_blksize = 1;
        return result;
    }
    return stat_for(m_node);
}

}
