/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/StringView.h>
#include <LibVultureFS/BTree.h>

namespace VultureFS {

// Nanoseconds since 1970.
using Clock = i64 (*)();

struct DirectoryEntry {
    InodeNumber inode;
    u8 type;
};

struct Unlinked {
    InodeNumber inode;
    // No names are left: call release() once nothing has it open.
    bool last_link;
};

struct Statistics {
    u64 generation;
    u64 block_count;
    u64 free_blocks;
    size_t tree_height;
};

// A mounted vultureFS volume: files, directories and symbolic links kept
// in one copy-on-write B+tree, with file data in extents. Changes collect
// in memory (file data is written at once, to blocks nothing on disk
// refers to yet) until checkpoint() commits them all at once.
//
// Not thread-safe: the kernel serializes calls.
class Volume {
public:
    static ErrorOr<NonnullOwnPtr<Volume>> format(BlockStore&, StringView name, ReadonlyBytes uuid, Clock);
    // Opens the newest intact checkpoint, then deletes orphans.
    static ErrorOr<NonnullOwnPtr<Volume>> mount(BlockStore&, Clock);

    [[nodiscard]] Statistics statistics() const;
    [[nodiscard]] StringView name() const;
    [[nodiscard]] Superblock const& superblock() const { return m_superblock; }
    [[nodiscard]] bool is_dirty() const { return m_dirty; }
    // After an I/O error during a checkpoint nothing more is written.
    [[nodiscard]] bool has_failed() const { return m_failed; }

    ErrorOr<InodeRecord> inode(InodeNumber);
    ErrorOr<DirectoryEntry> lookup(InodeNumber directory, StringView name);
    // `callback(StringView name, DirectoryEntry)` returns an Iteration.
    template<typename Callback>
    ErrorOr<void> for_each_entry(InodeNumber directory, Callback callback);

    // A file, directory or (with a target) symbolic link.
    ErrorOr<InodeNumber> create(InodeNumber directory, StringView name, u32 mode, u32 uid, u32 gid, StringView link_target = {});
    ErrorOr<void> link(InodeNumber directory, StringView name, InodeNumber);
    // unlink() for files and links; remove_directory() for empty
    // directories.
    ErrorOr<Unlinked> unlink(InodeNumber directory, StringView name);
    ErrorOr<Unlinked> remove_directory(InodeNumber directory, StringView name);
    // Replaces what `new_name` named, if anything; that is returned.
    ErrorOr<Optional<Unlinked>> rename(InodeNumber old_directory, StringView old_name, InodeNumber new_directory, StringView new_name);
    // Deletes an inode with no names left.
    ErrorOr<void> release(InodeNumber);

    ErrorOr<size_t> read(InodeNumber, u64 offset, Bytes);
    ErrorOr<void> write(InodeNumber, u64 offset, ReadonlyBytes);
    ErrorOr<void> truncate(InodeNumber, u64 size);
    ErrorOr<size_t> read_link(InodeNumber, Bytes);
    ErrorOr<void> set_permissions(InodeNumber, u32 permissions);
    ErrorOr<void> set_owner(InodeNumber, u32 uid, u32 gid);
    ErrorOr<void> set_times(InodeNumber, i64 access_time, i64 modification_time);

    // Commits everything changed since the last checkpoint.
    ErrorOr<void> checkpoint();

    // The physical block holding file block `file_block`, if any.
    ErrorOr<Optional<BlockIndex>> map_block(InodeNumber, u64 file_block);

private:
    struct Extent {
        u64 file_block;
        BlockIndex block;
        u64 count;
    };

    Volume(BlockStore&, Clock);

    ErrorOr<void> put_inode(InodeNumber, InodeRecord const&);
    ErrorOr<Optional<DirectoryEntry>> find_entry(InodeNumber directory, StringView name);
    ErrorOr<void> add_entry(InodeNumber directory, StringView name, DirectoryEntry);
    ErrorOr<void> remove_entry(InodeNumber directory, StringView name);
    ErrorOr<bool> is_empty_directory(InodeNumber);
    ErrorOr<InodeRecord> directory(InodeNumber);
    ErrorOr<Optional<Extent>> find_extent(InodeNumber, u64 file_block);
    ErrorOr<Vector<Extent>> extents_in(InodeNumber, u64 first, u64 end);
    // Unmaps file blocks [first, end), freeing their blocks; returns how
    // many were mapped.
    ErrorOr<u64> punch(InodeNumber, u64 first, u64 end);
    ErrorOr<void> add_extent(InodeNumber, Extent);
    ErrorOr<Unlinked> drop_link(InodeNumber directory, StringView name, InodeNumber, InodeRecord&);
    ErrorOr<bool> is_ancestor(InodeNumber ancestor, InodeNumber directory);
    // Changes a directory's entry count (its size) and link count, and
    // updates its times.
    ErrorOr<void> adjust_directory(InodeNumber, i64 entries, i64 links);
    // One block of a file, zeros for a hole.
    ErrorOr<void> read_file_block(InodeNumber, u64 file_block, u8* destination);
    ErrorOr<void> mutate();
    ErrorOr<void> mount_checkpoint(Superblock const&);
    ErrorOr<void> delete_orphans();
    [[nodiscard]] i64 now() const { return m_clock ? m_clock() : 0; }

    BlockStore& m_store;
    Clock m_clock;
    Allocator m_allocator;
    BTree m_tree;
    Superblock m_superblock {};
    bool m_dirty { false };
    bool m_failed { false };
};

template<typename Callback>
ErrorOr<void> Volume::for_each_entry(InodeNumber directory_inode, Callback callback)
{
    (void)TRY(directory(directory_inode));
    return m_tree.for_each_from(make_key(directory_inode, item_directory_entry, 0), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != directory_inode || key.type != item_directory_entry)
            return Iteration::Stop;
        for (size_t offset = 0; offset + sizeof(DirectoryEntryRecord) <= value.size();) {
            DirectoryEntryRecord record;
            __builtin_memcpy(&record, value.data() + offset, sizeof(record));
            offset += sizeof(record);
            if (offset + record.name_length > value.size())
                return Iteration::Stop;
            StringView name { reinterpret_cast<char const*>(value.data() + offset), record.name_length };
            offset += record.name_length;
            if (callback(name, DirectoryEntry { record.inode, record.type }) == Iteration::Stop)
                return Iteration::Stop;
        }
        return Iteration::Continue;
    });
}

}
