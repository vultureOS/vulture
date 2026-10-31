/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/FileSystem.h>
#include <Kernel/FileSystem/Pipe.h>
#include <LibBase/New.h>

namespace Kernel {

FileDescription::FileDescription(int flags)
    : m_flags(flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC | O_NOFOLLOW))
{
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create(NonnullRefPtr<Inode> inode, int flags)
{
    auto* description = new (Base::nothrow) FileDescription(flags);
    if (!description)
        return Error::from_errno(ENOMEM);
    description->m_inode = move(inode);
    return adopt_ref(*description);
}

ErrorOr<NonnullRefPtr<FileDescription>> FileDescription::create_for_pipe(NonnullRefPtr<Pipe> pipe, bool is_writer)
{
    auto* description = new (Base::nothrow) FileDescription(is_writer ? O_WRONLY : O_RDONLY);
    if (!description)
        return Error::from_errno(ENOMEM);
    pipe->attach(is_writer);
    description->m_pipe = move(pipe);
    description->m_is_pipe_writer = is_writer;
    return adopt_ref(*description);
}

FileDescription::~FileDescription()
{
    if (m_pipe)
        m_pipe->detach(m_is_pipe_writer);
}

void FileDescription::set_status_flags(int flags)
{
    constexpr int changeable = O_APPEND | O_NONBLOCK;
    m_flags = (m_flags & ~changeable) | (flags & changeable);
}

ErrorOr<size_t> FileDescription::read(IOBuffer const& buffer, size_t count)
{
    if (!is_readable())
        return Error::from_errno(EBADF);
    if (m_pipe)
        return m_pipe->read(buffer, count, m_flags & O_NONBLOCK);
    auto metadata = m_inode->metadata();
    if (metadata.type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    // Devices have no offset to speak of.
    if (metadata.type == InodeType::CharacterDevice)
        return m_inode->read(0, count, buffer);
    MutexLocker locker(m_lock);
    size_t done = TRY(m_inode->read(m_offset, count, buffer));
    m_offset += done;
    return done;
}

ErrorOr<size_t> FileDescription::write(IOBuffer const& buffer, size_t count)
{
    if (!is_writable())
        return Error::from_errno(EBADF);
    if (m_pipe)
        return m_pipe->write(buffer, count, m_flags & O_NONBLOCK);
    auto metadata = m_inode->metadata();
    if (metadata.type == InodeType::CharacterDevice)
        return m_inode->write(0, count, buffer);
    MutexLocker locker(m_lock);
    if (m_flags & O_APPEND)
        m_offset = m_inode->metadata().size;
    size_t done = TRY(m_inode->write(m_offset, count, buffer));
    m_offset += done;
    return done;
}

ErrorOr<i64> FileDescription::seek(i64 offset, int origin)
{
    if (m_pipe || m_inode->metadata().type == InodeType::CharacterDevice)
        return Error::from_errno(ESPIPE);
    MutexLocker locker(m_lock);
    i64 base = 0;
    switch (origin) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = static_cast<i64>(m_offset);
        break;
    case SEEK_END:
        base = static_cast<i64>(m_inode->metadata().size);
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

ErrorOr<vulture_stat> FileDescription::stat() const
{
    if (m_pipe) {
        vulture_stat result {};
        result.st_mode = S_IFIFO | 0600;
        result.st_nlink = 1;
        result.st_size = static_cast<long long>(m_pipe->buffered());
        result.st_blksize = 4096;
        return result;
    }
    return m_inode->metadata().to_stat(m_inode->file_system().id());
}

ErrorOr<size_t> FileDescription::read_directory_entries(IOBuffer const& buffer, size_t size)
{
    if (!m_inode || !m_inode->is_directory())
        return Error::from_errno(ENOTDIR);
    MutexLocker locker(m_lock);

    // The offset counts entries. Records are gathered in a kernel buffer
    // first: traversal happens under filesystem locks, copying to user
    // memory must not.
    struct Collector final : DirectoryVisitor {
        u64 skip;
        u64 index { 0 };
        u8 records[4096];
        size_t used { 0 };
        size_t limit;
        bool full { false };

        VisitDecision visit(StringView name, InodeIndex inode, InodeType type) override
        {
            if (index < skip) {
                ++index;
                return VisitDecision::Continue;
            }
            size_t length = align_up<size_t>(sizeof(vulture_dirent) + name.length() + 1, 8);
            if (used + length > limit) {
                full = true;
                return VisitDecision::Stop;
            }
            auto* record = reinterpret_cast<vulture_dirent*>(records + used);
            __builtin_memset(record, 0, length);
            record->d_ino = inode;
            record->d_off = static_cast<long long>(index + 1);
            record->d_reclen = static_cast<unsigned short>(length);
            record->d_type = directory_entry_type(type);
            __builtin_memcpy(record->d_name, name.characters_without_null_termination(), name.length());
            used += length;
            ++index;
            return VisitDecision::Continue;
        }
    };
    auto* collector = new (Base::nothrow) Collector;
    if (!collector)
        return Error::from_errno(ENOMEM);
    collector->skip = m_offset;
    collector->limit = min(size, sizeof(collector->records));
    auto result = m_inode->traverse(*collector);
    size_t used = collector->used;
    u64 next = collector->index;
    if (!result.is_error() && used == 0 && collector->full)
        result = Error::from_errno(EINVAL); // Buffer too small for one record.
    if (!result.is_error())
        result = buffer.write(0, { collector->records, used });
    delete collector;
    TRY(result);
    m_offset = next;
    return used;
}

ErrorOr<void> FileDescription::truncate(u64 size)
{
    if (!m_inode || !is_writable())
        return Error::from_errno(EINVAL);
    return m_inode->truncate(size);
}

ErrorOr<void> FileDescription::sync()
{
    if (!m_inode)
        return {};
    TRY(m_inode->sync());
    return m_inode->file_system().sync();
}

}
