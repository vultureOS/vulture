/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/files.h>
#include <Kernel/FileSystem/IOBuffer.h>
#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/RefPtr.h>
#include <LibBase/StringView.h>

namespace Kernel {

class FileDescription;
class FileSystem;
class TTY;

using InodeIndex = u64;

enum class InodeType : u8 {
    File,
    Directory,
    SymbolicLink,
    CharacterDevice,
    BlockDevice,
    Fifo,
};

struct InodeMetadata {
    InodeIndex index { 0 };
    InodeType type { InodeType::File };
    u32 permissions { 0644 };
    u32 link_count { 1 };
    u32 uid { 0 };
    u32 gid { 0 };
    u64 size { 0 };
    i64 modification_time { 0 }; // Seconds since 1970.
    // Nanoseconds since 1970; 0 where a filesystem does not keep them
    // (then the modification time stands in).
    i64 access_time_ns { 0 };
    i64 modification_time_ns { 0 };
    i64 change_time_ns { 0 };
    i64 birth_time_ns { 0 };
    u64 device { 0 }; // For device inodes: which device.
    u32 block_size { 4096 };

    [[nodiscard]] u32 mode() const;
    [[nodiscard]] vulture_stat to_stat(u32 file_system_id) const;
};

[[nodiscard]] u8 directory_entry_type(InodeType);

enum class VisitDecision {
    Continue,
    Stop,
};

class DirectoryVisitor {
public:
    virtual VisitDecision visit(StringView name, InodeIndex, InodeType) = 0;

protected:
    ~DirectoryVisitor() = default;
};

// A file, directory, symbolic link or device in some filesystem. The
// defaults refuse what does not apply (reading a directory, looking up in
// a file...); each filesystem overrides what it supports and does its own
// locking.
class Inode : public RefCounted<Inode> {
public:
    virtual ~Inode() = default;

    [[nodiscard]] FileSystem& file_system() const { return m_file_system; }
    [[nodiscard]] InodeIndex index() const { return m_index; }

    [[nodiscard]] virtual InodeMetadata metadata() const = 0;
    [[nodiscard]] bool is_directory() const { return metadata().type == InodeType::Directory; }

    virtual ErrorOr<size_t> read(u64 offset, size_t count, IOBuffer const&);
    virtual ErrorOr<size_t> write(u64 offset, size_t count, IOBuffer const&);
    virtual ErrorOr<void> truncate(u64 size);

    virtual ErrorOr<NonnullRefPtr<Inode>> lookup(StringView name);
    virtual ErrorOr<void> traverse(DirectoryVisitor&);
    // Creates a child; `link_target` is for symbolic links.
    virtual ErrorOr<NonnullRefPtr<Inode>> create_child(StringView name, InodeType, u32 permissions, StringView link_target);
    // Removes the name (unlink, rmdir; the VFS checks which applies).
    virtual ErrorOr<void> remove_child(StringView name);
    // Adds another name for `inode` (rename within one filesystem).
    virtual ErrorOr<void> add_child(StringView name, Inode&);

    virtual ErrorOr<size_t> read_link(Bytes buffer);
    virtual ErrorOr<void> set_permissions(u32 permissions);
    virtual ErrorOr<void> set_owner(u32 uid, u32 gid);
    // Nanoseconds since 1970.
    virtual ErrorOr<void> set_times(i64 access_time, i64 modification_time);
    virtual ErrorOr<void> sync();

    // Memory mapping (mmap): the page holding page `index` of the file,
    // with a reference for the caller. Mappings of a file share these
    // pages with read() and write().
    virtual ErrorOr<PhysicalAddress> mapped_page(u64 index);
    // Devices mmap() works on (as all regular files do).
    [[nodiscard]] virtual bool can_be_mapped() const { return false; }
    // A shared, writable mapping of the file begins or ends; while one
    // exists its pages may change at any moment.
    virtual void add_shared_writer() { }
    virtual void remove_shared_writer() { }

    // Device files: what open() really opens (/dev/ptmx makes a new
    // terminal pair and opens its master end).
    virtual ErrorOr<NonnullRefPtr<Inode>> open_inode(int flags);
    virtual ErrorOr<FlatPtr> ioctl(FileDescription&, unsigned request, FlatPtr argument);
    [[nodiscard]] virtual TTY* as_tty() { return nullptr; }
    // For poll(): whether a read or a write would not block.
    [[nodiscard]] virtual bool can_read() const { return true; }
    [[nodiscard]] virtual bool can_write() const { return true; }
    // And whoever is at the other end has gone.
    [[nodiscard]] virtual bool is_hung_up() const { return false; }
    // An open file description of it is made, or the last reference to one goes. (May
    // be called with spinlocks held, so these must not sleep.)
    virtual void description_opened() { }
    virtual void description_closed() { }

    // Whether a directory has no entries besides "." and "..".
    [[nodiscard]] ErrorOr<bool> is_empty_directory();

protected:
    Inode(FileSystem& file_system, InodeIndex index)
        : m_file_system(file_system)
        , m_index(index)
    {
    }

private:
    FileSystem& m_file_system;
    InodeIndex m_index;
};

}
