/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/String.h>
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
    // Not in use, but a snapshot has them.
    u64 snapshot_blocks;
};

struct FormatOptions {
    bool case_sensitive { false };
    VolumeRole role { role_none };
    u8 const* group_uuid { nullptr };
    // Made once, and only read from then on (LibVultureFS/Sealed.h).
    bool sealed { false };
};

struct MountOptions {
    bool read_only { false };
};

enum class XattrMode : u8 {
    Either,
    MustBeNew,
    MustExist,
};

struct SnapshotInfo {
    u64 id;
    String name;
    i64 time;
    u64 generation;
};

// A block of a file was not what its checksum said it should be.
using DamageReporter = void (*)(InodeNumber, u64 file_block, BlockIndex);

// A mounted vultureFS volume: files, directories and symbolic links kept
// in one copy-on-write B+tree, with file data in extents. Changes collect
// in memory (file data is written at once, to blocks nothing on disk
// refers to yet) until checkpoint() commits them all at once.
//
// Not thread-safe: the kernel serializes calls.
class Volume {
public:
    static ErrorOr<NonnullOwnPtr<Volume>> format(BlockStore&, StringView name, ReadonlyBytes uuid, Clock, FormatOptions = {});
    // Opens the newest intact checkpoint, goes back to a snapshot if that's been asked for, then deletes orphans.
    static ErrorOr<NonnullOwnPtr<Volume>> mount(BlockStore&, Clock, MountOptions = {});
    // What a snapshot has, which can't be changed.
    static ErrorOr<NonnullOwnPtr<Volume>> mount_snapshot(BlockStore&, Clock, StringView name);

    [[nodiscard]] Statistics statistics() const;
    [[nodiscard]] StringView name() const;
    [[nodiscard]] Superblock const& superblock() const { return m_superblock; }
    [[nodiscard]] bool is_dirty() const { return m_dirty; }
    [[nodiscard]] bool is_read_only() const { return m_read_only; }
    [[nodiscard]] bool is_case_insensitive() const { return m_superblock.features_required & feature_case_insensitive; }
    // After an I/O error during a checkpoint nothing more is written.
    [[nodiscard]] bool has_failed() const { return m_failed; }
    void set_damage_reporter(DamageReporter reporter) { m_damage_reporter = reporter; }

    ErrorOr<InodeRecord> inode(InodeNumber);
    ErrorOr<DirectoryEntry> lookup(InodeNumber directory, StringView name);
    // `callback(StringView name, DirectoryEntry)` returns an Iteration.
    template<typename Callback>
    ErrorOr<void> for_each_entry(InodeNumber directory, Callback callback);

    // A file, directory, named pipe, socket or (with a target) symbolic link.
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
    // st_flags, for those that are simply remembered.
    ErrorOr<void> set_bsd_flags(InodeNumber, u32);

    // Whether a file's data is kept compressed: what's there already changes to suit.
    ErrorOr<void> set_compressed(InodeNumber, bool);

    // A copy that shares the original's blocks until one or the other changes: of a whole
    // tree, for a directory. Keeps everything of the original's but its owner, unless that's the same.
    ErrorOr<InodeNumber> clone(InodeNumber source, InodeNumber directory, StringView name, u32 uid, u32 gid);

    // ---- Extended attributes.
    // Its value, or as much as `buffer` has room for: with an empty one, only its size.
    // ENOATTR-equivalent is `Error::from_errno(ENOATTR)`.
    ErrorOr<size_t> get_xattr(InodeNumber, StringView name, Bytes buffer);
    ErrorOr<void> set_xattr(InodeNumber, StringView name, ReadonlyBytes value, XattrMode = XattrMode::Either);
    ErrorOr<void> remove_xattr(InodeNumber, StringView name);
    // `callback(StringView name, u64 size)` returns an Iteration.
    template<typename Callback>
    ErrorOr<void> for_each_xattr(InodeNumber, Callback callback);

    // ---- Access control lists.
    ErrorOr<Vector<AclEntry>> acl(InodeNumber);
    ErrorOr<void> set_acl(InodeNumber, Span<AclEntry const>);

    // ---- Snapshots: this moment of the volume, kept. These commit by themselves.
    ErrorOr<void> create_snapshot(StringView name);
    ErrorOr<void> delete_snapshot(StringView name);
    ErrorOr<Vector<SnapshotInfo>> snapshots();
    // The next mount goes back to it, exactly as it was. It stays, and those made after it go.
    ErrorOr<void> request_rollback(StringView name);

    // ---- What's changed: kept with the changes, so that what's committed is what's said.
    ErrorOr<u64> append_event(u32 flags, StringView path);
    [[nodiscard]] u64 next_event() const { return m_superblock.next_event; }
    [[nodiscard]] u64 oldest_event() const { return m_superblock.oldest_event; }
    // `callback(u64 id, u32 flags, StringView path)` returns an Iteration.
    template<typename Callback>
    ErrorOr<void> for_each_event(u64 first, Callback callback);

    // Commits everything changed since the last checkpoint.
    ErrorOr<void> checkpoint();

    // The physical block holding file block `file_block`, if any and if it's one block's worth of plain data.
    ErrorOr<Optional<BlockIndex>> map_block(InodeNumber, u64 file_block);

private:
    struct Extent {
        u64 file_block;
        BlockIndex block;
        u64 count;   // On the disk.
        u64 logical; // Of the file.
        u8 compression;
        u32 compressed_size;
    };

    Volume(BlockStore&, Clock);

    ErrorOr<void> put_inode(InodeNumber, InodeRecord const&);
    ErrorOr<Key> entry_key(InodeNumber directory, StringView name, Vector<u8>& folded);
    ErrorOr<Optional<DirectoryEntry>> find_entry(InodeNumber directory, StringView name, Vector<u8>* spelling = nullptr);
    ErrorOr<void> add_entry(InodeNumber directory, StringView name, DirectoryEntry);
    ErrorOr<void> remove_entry(InodeNumber directory, StringView name);
    ErrorOr<bool> is_empty_directory(InodeNumber);
    ErrorOr<InodeRecord> directory(InodeNumber);
    ErrorOr<Optional<Extent>> find_extent(InodeNumber, u64 file_block);
    ErrorOr<Vector<Extent>> extents_in(InodeNumber, u64 first, u64 end);
    // Unmaps file blocks [first, end), letting go of their blocks; returns how
    // many there were on the disk.
    ErrorOr<u64> punch(InodeNumber, u64 first, u64 end);
    ErrorOr<void> add_extent(InodeNumber, Extent);
    ErrorOr<void> put_extent(InodeNumber, Extent const&);
    ErrorOr<Unlinked> drop_link(InodeNumber directory, StringView name, InodeNumber, InodeRecord&);
    ErrorOr<bool> is_ancestor(InodeNumber ancestor, InodeNumber directory);
    // Changes a directory's entry count (its size) and link count, and
    // updates its times.
    ErrorOr<void> adjust_directory(InodeNumber, i64 entries, i64 links);
    // One block of a file, zeros for a hole.
    ErrorOr<void> read_file_block(InodeNumber, u64 file_block, u8* destination);
    ErrorOr<void> read_verified(InodeNumber, u64 file_block, BlockIndex, u64 count, u8* destination);
    ErrorOr<void> mutate();
    ErrorOr<void> mount_checkpoint(Superblock const&);
    ErrorOr<void> load_snapshots();
    ErrorOr<void> delete_orphans();
    ErrorOr<void> go_back(u64 snapshot);
    ErrorOr<Optional<Superblock>> saved_superblock(u64 snapshot);
    ErrorOr<Optional<u64>> find_snapshot(StringView name);
    [[nodiscard]] u64 blocks_to_keep_back() const;
    [[nodiscard]] i64 now() const { return m_clock ? m_clock() : 0; }

    // Blocks now shared by one more, or by one fewer: those nothing has any more are freed.
    ErrorOr<void> add_references(BlockIndex first, u64 count);
    ErrorOr<void> drop_references(BlockIndex first, u64 count);
    ErrorOr<void> set_checksums(BlockIndex first, u64 count, u8 const* data);
    ErrorOr<void> clear_checksums(BlockIndex first, u64 count);
    // False if a block's not what was written.
    ErrorOr<Optional<u64>> find_damage(BlockIndex first, u64 count, u8 const* data);

    ErrorOr<void> write_plain(InodeNumber, u64 offset, ReadonlyBytes, InodeRecord&);
    ErrorOr<void> write_compressed(InodeNumber, u64 offset, ReadonlyBytes, InodeRecord&);
    // One chunk's worth, whatever was there.
    ErrorOr<void> put_chunk(InodeNumber, u64 chunk_start_block, u8 const* data, size_t size, InodeRecord&);
    ErrorOr<void> read_chunk(InodeNumber, Extent const&);
    ErrorOr<size_t> read_any(InodeNumber, u64 offset, Bytes, InodeRecord const&);
    void forget_chunk() { m_chunk_inode = 0; }

    ErrorOr<void> copy_contents(InodeNumber source, InodeNumber target, InodeRecord const&);
    ErrorOr<InodeNumber> clone_inode(InodeNumber source, u32 uid, u32 gid, InodeNumber parent);
    ErrorOr<void> release_extras(InodeNumber);

    BlockStore& m_store;
    Clock m_clock;
    Allocator m_allocator;
    BTree m_tree;
    Superblock m_superblock {};
    bool m_dirty { false };
    bool m_failed { false };
    bool m_read_only { false };
    DamageReporter m_damage_reporter { nullptr };
    // The compressed chunk last read, since reads come in smaller pieces than these.
    InodeNumber m_chunk_inode { 0 };
    u64 m_chunk_block { 0 };
    Vector<u8> m_chunk;
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

template<typename Callback>
ErrorOr<void> Volume::for_each_xattr(InodeNumber number, Callback callback)
{
    return m_tree.for_each_from(make_key(number, item_xattr, 0), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != number || key.type != item_xattr)
            return Iteration::Stop;
        for (size_t offset = 0; offset + sizeof(XattrRecord) <= value.size();) {
            XattrRecord record;
            __builtin_memcpy(&record, value.data() + offset, sizeof(record));
            offset += sizeof(record);
            if (offset + record.name_length + record.inline_size > value.size())
                return Iteration::Stop;
            StringView name { reinterpret_cast<char const*>(value.data() + offset), record.name_length };
            offset += record.name_length + record.inline_size;
            u64 const size = (record.flags & xattr_in_stream) ? record.stream_size : record.inline_size;
            if (callback(name, size) == Iteration::Stop)
                return Iteration::Stop;
        }
        return Iteration::Continue;
    });
}

template<typename Callback>
ErrorOr<void> Volume::for_each_event(u64 first, Callback callback)
{
    return m_tree.for_each_from(make_key(events_object, item_event, first), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != events_object || key.type != item_event)
            return Iteration::Stop;
        if (value.size() < sizeof(EventRecord))
            return Iteration::Continue;
        EventRecord record;
        __builtin_memcpy(&record, value.data(), sizeof(record));
        StringView path { reinterpret_cast<char const*>(value.data() + sizeof(record)), value.size() - sizeof(record) };
        return callback(key.offset, record.flags, path);
    });
}

}
