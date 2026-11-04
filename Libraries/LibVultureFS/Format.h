/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// The on-disk format of vultureFS, version 2. All integers are
// little-endian. See Documentation/VultureFS.md.
//
// A volume is an array of 4 KiB blocks:
//
//   block 0, 1    superblocks; checkpoint N is written to block N % 2
//   the rest      B+tree nodes, allocation bitmap blocks and file data,
//                 wherever the allocator put them
//
// Nothing a checkpoint or a snapshot refers to is ever overwritten until a
// later checkpoint has committed without it, so a crash at any moment leaves
// the last committed checkpoint intact.

namespace VultureFS {

constexpr size_t block_size = 4096;
constexpr u32 format_version = 2;
constexpr char superblock_magic[8] = { 'v', 'u', 'l', 't', 'u', 'r', 'e', '2' };
// What version 1 said, which is only looked for to say so clearly.
constexpr char old_superblock_magic[8] = { 'v', 'u', 'l', 't', 'u', 'r', 'e', '1' };
constexpr u32 node_magic = 0x4e53'4656; // "VFSN"

using BlockIndex = u64;
using InodeNumber = u64;

// Object identifiers below this are reserved.
constexpr InodeNumber orphan_list_object = 1;
constexpr InodeNumber root_directory = 2;
constexpr InodeNumber shared_blocks_object = 3;
constexpr InodeNumber checksums_object = 4;
constexpr InodeNumber events_object = 5;
constexpr InodeNumber snapshots_object = 6;
constexpr InodeNumber first_free_object = 16;

// Each bitmap block holds the bits of this many blocks.
constexpr u64 blocks_per_bitmap_block = block_size * 8;

struct [[gnu::packed]] BitmapPointer {
    u64 block;
    u32 checksum; // CRC-32C of the block it points to.
    u32 reserved;
};
static_assert(sizeof(BitmapPointer) == 16);

// Where the bitmap blocks are: in these, which the superblock points to.
constexpr size_t bitmap_pointers_per_index_block = block_size / sizeof(BitmapPointer);
constexpr size_t max_bitmap_index_blocks = 240;
constexpr u64 max_volume_blocks = max_bitmap_index_blocks * bitmap_pointers_per_index_block * blocks_per_bitmap_block; // 8 PiB.

// What a volume needs of whatever reads it. Something that doesn't know one of the
// `features_required` has to leave the volume alone, and one of `features_read_only`
// can be read but not changed; the rest make no difference to anyone else.
constexpr u64 feature_case_insensitive = 1ull << 0; // Required: names are looked up that way.
constexpr u64 feature_sealed = 1ull << 1;           // Read-only: a system volume, with a signed hash tree after it.
constexpr u64 feature_encrypted = 1ull << 2;        // Required: M43. Extents say which key.

constexpr u64 known_features_required = feature_case_insensitive;
constexpr u64 known_features_read_only = feature_sealed;
constexpr u64 known_features_compatible = 0;

// What a volume is for, also said by its partition's type on a GPT disk.
enum VolumeRole : u32 {
    role_none = 0,
    role_system = 1,
    role_data = 2,
};

struct [[gnu::packed]] Superblock {
    char magic[8];
    u32 checksum; // CRC-32C of the whole block, taken with this field zero.
    u32 version;
    u64 generation; // Checkpoint number, from 1.
    u64 block_count;
    u64 free_blocks; // That this checkpoint doesn't use: a snapshot may be holding on to some.
    u64 tree_root;
    u64 tree_root_generation;
    u64 next_object;
    i64 created_time; // Nanoseconds since 1970.
    i64 checkpoint_time;
    u8 uuid[16];
    char name[64];
    u32 bitmap_block_count;
    u32 role;
    u64 features_required;
    u64 features_read_only;
    u64 features_compatible;
    u64 next_event;
    u64 oldest_event;
    u64 next_snapshot;
    u64 pending_rollback; // The snapshot the next mount goes back to, if not 0.
    u8 group_uuid[16];    // The same in a system and its data.
    u8 reserved[256 - 240];
    BitmapPointer bitmap_index[max_bitmap_index_blocks];
};
static_assert(sizeof(Superblock) == block_size);
static_assert(__builtin_offsetof(Superblock, bitmap_index) == 256);

// The tree's key: items sort by object, then type, then offset.
struct [[gnu::packed]] Key {
    u64 object;
    u64 offset;
    u32 type;
    u32 reserved;

    constexpr int compare(Key const& other) const
    {
        if (object != other.object)
            return object < other.object ? -1 : 1;
        if (type != other.type)
            return type < other.type ? -1 : 1;
        if (offset != other.offset)
            return offset < other.offset ? -1 : 1;
        return 0;
    }
    constexpr bool operator==(Key const& other) const { return compare(other) == 0; }
    constexpr bool operator<(Key const& other) const { return compare(other) < 0; }
    constexpr bool operator<=(Key const& other) const { return compare(other) <= 0; }
};
static_assert(sizeof(Key) == 24);

constexpr Key make_key(u64 object, u32 type, u64 offset) { return { object, offset, type, 0 }; }

enum ItemType : u32 {
    // (inode, Inode, 0) -> InodeRecord
    item_inode = 1,
    // (directory, DirectoryEntry, hash of the folded name) -> DirectoryEntryRecord...
    // Names whose hashes collide share an item.
    item_directory_entry = 2,
    // (inode, Extent, first file block) -> ExtentRecord
    item_extent = 3,
    // (inode, InlineData, 0) -> bytes; a symbolic link's target
    item_inline_data = 4,
    // (orphan_list_object, Orphan, inode) -> nothing; an inode with no
    // names left that was still open. Mounting deletes these.
    item_orphan = 5,
    // (inode, Xattr, hash of the name) -> XattrRecord...
    item_xattr = 6,
    // (inode, Acl, 0) -> AclEntry...
    item_acl = 7,
    // (shared_blocks_object, SharedBlocks, first block) -> SharedBlocksRecord: blocks with
    // more than one file's extents on them. Blocks with one have no such item.
    item_shared_blocks = 8,
    // (checksums_object, Checksums, first block of the group) -> u32[checksums_per_item]
    item_checksums = 9,
    // (events_object, Event, id) -> EventRecord and the path
    item_event = 10,
    // (snapshots_object, Snapshot, id) -> SnapshotRecord and the name
    item_snapshot = 11,
};

struct [[gnu::packed]] NodeHeader {
    u32 magic;
    u32 checksum; // CRC-32C of the whole block, taken with this field zero.
    u64 block;    // Where it was written: catches misdirected writes.
    u64 generation;
    u16 level; // 0 for leaves.
    u16 count;
    u32 reserved;
};
static_assert(sizeof(NodeHeader) == 32);

// Leaves: an array of these after the header; values are packed from the
// end of the block towards it.
struct [[gnu::packed]] LeafEntry {
    Key key;
    u16 value_offset;
    u16 value_size;
};
static_assert(sizeof(LeafEntry) == 28);

// Internal nodes: the key is the lowest key in the child's subtree (or
// lower, for the first child).
struct [[gnu::packed]] InternalEntry {
    Key key;
    u64 child;
    u64 child_generation;
};
static_assert(sizeof(InternalEntry) == 40);

constexpr size_t node_capacity = block_size - sizeof(NodeHeader);
constexpr size_t max_internal_entries = node_capacity / sizeof(InternalEntry);

// File types, as in the st_mode of stat().
constexpr u32 mode_type_mask = 0170000;
constexpr u32 mode_directory = 0040000;
constexpr u32 mode_regular = 0100000;
constexpr u32 mode_symbolic_link = 0120000;
constexpr u32 mode_fifo = 0010000;
constexpr u32 mode_socket = 0140000;

// In an inode's flags.
constexpr u32 inode_flag_compressed = 1u << 0; // Its data is in compressed chunks.
constexpr u32 inode_flag_stream = 1u << 1;     // It's the value of an extended attribute, and has no name.
constexpr u32 inode_flag_has_acl = 1u << 2;    // So that it's only looked up when there's something to find.

struct [[gnu::packed]] InodeRecord {
    u32 mode;
    u32 link_count;
    u32 uid;
    u32 gid;
    u64 size;
    u64 allocated_blocks; // On the disk, however many others also have them.
    i64 access_time;      // Nanoseconds since 1970.
    i64 modification_time;
    i64 change_time;
    i64 birth_time;
    u64 parent; // Directories: the directory holding it.
    u32 flags;
    u32 reserved; // The BSD flags (st_flags), when something uses them.
};
static_assert(sizeof(InodeRecord) == 80);

struct [[gnu::packed]] DirectoryEntryRecord {
    u64 inode;
    u8 type; // DT_* as in dirent.h
    u8 reserved;
    u16 name_length;
    // Followed by the name, as it was given.
};

constexpr size_t max_name_length = 255;
constexpr size_t max_link_target = 1023;

constexpr u8 compression_none = 0;
constexpr u8 compression_deflate = 1;
// A compressed file's data, in pieces of this many of its blocks, each one an extent.
constexpr u64 compression_chunk_blocks = 16;

struct [[gnu::packed]] ExtentRecord {
    u64 block;
    u64 count;          // Blocks on the disk.
    u32 logical_count;  // File blocks it holds: 0 for as many as it has on the disk.
    u8 compression;     // compression_*
    u8 reserved[3];
    u32 encryption_key; // Which of the volume's keys, for M43: 0 for none.
    u32 compressed_size; // In bytes, of what's in those blocks, if it's compressed.
};
static_assert(sizeof(ExtentRecord) == 32);

struct [[gnu::packed]] SharedBlocksRecord {
    u64 count;
    u64 references; // At least 2.
};
static_assert(sizeof(SharedBlocksRecord) == 16);

// A CRC-32C for each of this many blocks, the first of them a multiple of it: 0 for a
// block that isn't file data, and a block whose CRC really is 0 says 1.
constexpr u64 checksums_per_item = 256;

constexpr size_t max_xattr_name_length = 127;
// Values longer than this are in a file of their own, with no name.
constexpr size_t max_inline_xattr_size = 960;

constexpr u16 xattr_in_stream = 1;

struct [[gnu::packed]] XattrRecord {
    u16 name_length;
    u16 flags;
    u32 inline_size;
    u64 stream_inode;
    u64 stream_size;
    // Followed by the name, and the value if it's inline.
};
static_assert(sizeof(XattrRecord) == 24);

// Who it's about, what it says about them and what's to happen to new things in a directory
// with this: as a Mac's ACLs are, with numbers for users and groups.
constexpr u8 acl_allow = 0;
constexpr u8 acl_deny = 1;
constexpr u8 acl_user = 0;
constexpr u8 acl_group = 1;
constexpr u8 acl_everyone = 2;

constexpr u32 acl_read_data = 1u << 1;        // A directory's: list.
constexpr u32 acl_write_data = 1u << 2;       // A directory's: add_file.
constexpr u32 acl_execute = 1u << 3;          // A directory's: search.
constexpr u32 acl_delete = 1u << 4;
constexpr u32 acl_append_data = 1u << 5;      // A directory's: add_subdirectory.
constexpr u32 acl_delete_child = 1u << 6;
constexpr u32 acl_read_attributes = 1u << 7;
constexpr u32 acl_write_attributes = 1u << 8;
constexpr u32 acl_read_extattributes = 1u << 9;
constexpr u32 acl_write_extattributes = 1u << 10;
constexpr u32 acl_read_security = 1u << 11;
constexpr u32 acl_write_security = 1u << 12;
constexpr u32 acl_change_owner = 1u << 13;

constexpr u16 acl_entry_inherited = 1u << 0;
constexpr u16 acl_file_inherit = 1u << 4;
constexpr u16 acl_directory_inherit = 1u << 5;
constexpr u16 acl_limit_inherit = 1u << 6;
constexpr u16 acl_only_inherit = 1u << 7;

struct [[gnu::packed]] AclEntry {
    u8 kind;  // acl_allow, acl_deny
    u8 tag;   // acl_user, acl_group, acl_everyone
    u16 inheritance;
    u32 id;
    u32 permissions;
};
static_assert(sizeof(AclEntry) == 12);

constexpr size_t max_acl_entries = 96;

struct [[gnu::packed]] EventRecord {
    u32 flags; // FSEvents' flags, as a Mac has them.
    u32 reserved;
    // Followed by the path.
};

// The newest of these are kept.
constexpr u64 max_kept_events = 32768;

struct [[gnu::packed]] SnapshotRecord {
    u64 saved_superblock; // A block that has a copy of the checkpoint's superblock in it,
    u32 saved_checksum;   // whose CRC-32C is this.
    u32 reserved;
    i64 time;
    u64 generation;
    // Followed by the name.
};

constexpr size_t max_snapshot_name_length = 255;

// FSEvents' flags. These are a Mac's own numbers.
constexpr u32 event_must_scan_subdirectories = 0x00000001;
constexpr u32 event_history_done = 0x00000010;
constexpr u32 event_root_changed = 0x00000020;
constexpr u32 event_mount = 0x00000040;
constexpr u32 event_unmount = 0x00000080;
constexpr u32 event_item_created = 0x00000100;
constexpr u32 event_item_removed = 0x00000200;
constexpr u32 event_item_inode_metadata_changed = 0x00000400;
constexpr u32 event_item_renamed = 0x00000800;
constexpr u32 event_item_modified = 0x00001000;
constexpr u32 event_item_owner_changed = 0x00004000;
constexpr u32 event_item_xattr_changed = 0x00008000;
constexpr u32 event_item_is_file = 0x00010000;
constexpr u32 event_item_is_directory = 0x00020000;
constexpr u32 event_item_is_symbolic_link = 0x00040000;
constexpr u32 event_item_cloned = 0x00400000;

// DT_* values.
constexpr u8 type_directory = 4;
constexpr u8 type_regular = 8;
constexpr u8 type_symbolic_link = 10;
constexpr u8 type_fifo = 1;
constexpr u8 type_socket = 12;

constexpr u8 directory_type_for_mode(u32 mode)
{
    switch (mode & mode_type_mask) {
    case mode_directory:
        return type_directory;
    case mode_symbolic_link:
        return type_symbolic_link;
    case mode_fifo:
        return type_fifo;
    case mode_socket:
        return type_socket;
    default:
        return type_regular;
    }
}

// FNV-1a, folded to 63 bits so it never collides with the reserved top.
constexpr u64 name_hash(char const* name, size_t length)
{
    u64 hash = 0xcbf2'9ce4'8422'2325ull;
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<u8>(name[i]);
        hash *= 0x100'0000'01b3ull;
    }
    return hash >> 1;
}

// GPT partition types, stored mixed-endian as GPT does.
// 7675c7e1-56d1-4c4f-8f5e-76756c747572: a volume of neither kind, which is what version 1 used.
constexpr u8 partition_type[16] = { 0xe1, 0xc7, 0x75, 0x76, 0xd1, 0x56, 0x4f, 0x4c, 0x8f, 0x5e, 0x76, 0x75, 0x6c, 0x74, 0x75, 0x72 };
// 7675c7e1-56d1-4c4f-8f5e-76756c747573: a system.
constexpr u8 system_partition_type[16] = { 0xe1, 0xc7, 0x75, 0x76, 0xd1, 0x56, 0x4f, 0x4c, 0x8f, 0x5e, 0x76, 0x75, 0x6c, 0x74, 0x75, 0x73 };
// 7675c7e1-56d1-4c4f-8f5e-76756c747564: a system's data.
constexpr u8 data_partition_type[16] = { 0xe1, 0xc7, 0x75, 0x76, 0xd1, 0x56, 0x4f, 0x4c, 0x8f, 0x5e, 0x76, 0x75, 0x6c, 0x74, 0x75, 0x64 };

}
