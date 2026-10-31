/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// The on-disk format of vultureFS, version 1. All integers are
// little-endian. See Documentation/VultureFS.md.
//
// A volume is an array of 4 KiB blocks:
//
//   block 0, 1    superblocks; checkpoint N is written to block N % 2
//   the rest      B+tree nodes, allocation bitmap blocks and file data,
//                 wherever the allocator put them
//
// Nothing a checkpoint refers to is ever overwritten until a later
// checkpoint has committed without it, so a crash at any moment leaves the
// last committed checkpoint intact.

namespace VultureFS {

constexpr size_t block_size = 4096;
constexpr u32 format_version = 1;
constexpr char superblock_magic[8] = { 'v', 'u', 'l', 't', 'u', 'r', 'e', '1' };
constexpr u32 node_magic = 0x4e53'4656; // "VFSN"

using BlockIndex = u64;
using InodeNumber = u64;

// Object identifiers below this are reserved.
constexpr InodeNumber orphan_list_object = 1;
constexpr InodeNumber root_directory = 2;
constexpr InodeNumber first_free_object = 16;

// Each bitmap block holds the bits of this many blocks.
constexpr u64 blocks_per_bitmap_block = block_size * 8;
constexpr size_t max_bitmap_blocks = 224;
// Volumes up to 28 GiB.
constexpr u64 max_volume_blocks = max_bitmap_blocks * blocks_per_bitmap_block;

struct [[gnu::packed]] BitmapPointer {
    u64 block;
    u32 checksum; // CRC-32C of the bitmap block.
    u32 reserved;
};

struct [[gnu::packed]] Superblock {
    char magic[8];
    u32 checksum; // CRC-32C of the whole block, taken with this field zero.
    u32 version;
    u64 generation; // Checkpoint number, from 1.
    u64 block_count;
    u64 free_blocks;
    u64 tree_root;
    u64 tree_root_generation;
    u64 next_object;
    i64 created_time; // Nanoseconds since 1970.
    i64 checkpoint_time;
    u8 uuid[16];
    char name[64];
    u32 bitmap_block_count;
    u32 flags;
    u8 reserved[256 - 168];
    BitmapPointer bitmap[max_bitmap_blocks];
};
static_assert(sizeof(Superblock) <= block_size);
static_assert(__builtin_offsetof(Superblock, bitmap) == 256);

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
    // (directory, DirectoryEntry, hash of the name) -> DirectoryEntryRecord...
    // Names whose hashes collide share an item.
    item_directory_entry = 2,
    // (inode, Extent, first file block) -> ExtentRecord
    item_extent = 3,
    // (inode, InlineData, 0) -> bytes; a symbolic link's target
    item_inline_data = 4,
    // (orphan_list_object, Orphan, inode) -> nothing; an inode with no
    // names left that was still open. Mounting deletes these.
    item_orphan = 5,
};

struct [[gnu::packed]] NodeHeader {
    u32 magic;
    u32 checksum; // CRC-32C of the whole block, taken with this field zero.
    u64 block; // Where it was written: catches misdirected writes.
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

struct [[gnu::packed]] InodeRecord {
    u32 mode;
    u32 link_count;
    u32 uid;
    u32 gid;
    u64 size;
    u64 allocated_blocks;
    i64 access_time; // Nanoseconds since 1970.
    i64 modification_time;
    i64 change_time;
    i64 birth_time;
    u64 parent; // Directories: the directory holding it.
    u32 flags;
    u32 reserved;
};
static_assert(sizeof(InodeRecord) == 80);

struct [[gnu::packed]] DirectoryEntryRecord {
    u64 inode;
    u8 type; // DT_* as in dirent.h
    u8 reserved;
    u16 name_length;
    // Followed by the name.
};

constexpr size_t max_name_length = 255;
constexpr size_t max_link_target = 1023;

struct [[gnu::packed]] ExtentRecord {
    u64 block;
    u64 count;
};

// DT_* values.
constexpr u8 type_directory = 4;
constexpr u8 type_regular = 8;
constexpr u8 type_symbolic_link = 10;

constexpr u8 directory_type_for_mode(u32 mode)
{
    switch (mode & mode_type_mask) {
    case mode_directory:
        return type_directory;
    case mode_symbolic_link:
        return type_symbolic_link;
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

// The GPT partition type of a vultureFS volume:
// 7675c7e1-56d1-4c4f-8f5e-76756c747572, stored mixed-endian as GPT does.
constexpr u8 partition_type[16] = { 0xe1, 0xc7, 0x75, 0x76, 0xd1, 0x56, 0x4f, 0x4c, 0x8f, 0x5e, 0x76, 0x75, 0x6c, 0x74, 0x75, 0x72 };

}
