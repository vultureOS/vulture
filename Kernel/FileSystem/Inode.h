/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/files.h>
#include <Kernel/FileSystem/IOBuffer.h>
#include <LibBase/RefPtr.h>
#include <LibBase/StringView.h>

namespace Kernel {

class FileSystem;

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
    virtual ErrorOr<void> sync();

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
