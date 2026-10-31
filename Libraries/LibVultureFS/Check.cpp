/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Checksum.h>
#include <LibBase/HashMap.h>
#include <LibVultureFS/Allocator.h>
#include <LibVultureFS/BTree.h>
#include <LibVultureFS/Check.h>

namespace VultureFS {

namespace {

constexpr size_t max_reported_problems = 100;
constexpr size_t max_depth = 32;

struct InodeInfo {
    InodeRecord record {};
    u64 references { 0 };
    u64 subdirectories { 0 };
    u64 entries { 0 };
    u64 extent_blocks { 0 };
    u64 extent_end { 0 }; // One past the last mapped file block.
    bool has_inline { false };
    bool orphan { false };
    bool reachable { false };
};

class Checker {
public:
    Checker(BlockStore& store, CheckReport& report)
        : m_store(store)
        , m_report(report)
    {
    }

    ErrorOr<void> run();

private:
    template<typename... Args>
    void problem(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
    {
        ++m_problem_count;
        if (m_report.problems.size() >= max_reported_problems)
            return;
        StringBuilder builder;
        builder.appendff(format, args...);
        auto text = builder.to_string();
        if (!text.is_error())
            m_report.problems.append(text.release_value());
    }

    void reference(BlockIndex block, u64 count, StringView what)
    {
        for (u64 i = 0; i < count; ++i) {
            BlockIndex index = block + i;
            if (index >= m_report.block_count) {
                problem("{} uses block {} past the end of the volume", what, index);
                return;
            }
            u64& word = m_referenced[index / 64];
            u64 mask = 1ull << (index % 64);
            if (word & mask)
                problem("block {} is used twice (again by {})", index, what);
            word |= mask;
        }
    }

    bool read_superblock(Bytes block, Superblock&);
    ErrorOr<void> check_node(BlockIndex, u64 generation, u16 level, Key const& low, Optional<Key> const& high, size_t depth);
    void check_items();
    void check_inodes(Superblock const&);

    BlockStore& m_store;
    CheckReport& m_report;
    size_t m_problem_count { 0 };
    u64 m_generation { 0 };
    Vector<u64> m_referenced;
    Vector<Item> m_items;
    HashMap<u64, InodeInfo> m_inodes;
    HashMap<u64, Vector<u64>> m_children;
};

bool Checker::read_superblock(Bytes block, Superblock& superblock)
{
    __builtin_memcpy(&superblock, block.data(), sizeof(superblock));
    if (__builtin_memcmp(superblock.magic, superblock_magic, sizeof(superblock.magic)) != 0)
        return false;
    u32 checksum = superblock.checksum;
    __builtin_memset(block.data() + __builtin_offsetof(Superblock, checksum), 0, sizeof(u32));
    if (crc32c(block) != checksum)
        return false;
    return superblock.version == format_version && superblock.generation >= 1 && superblock.block_count <= m_store.block_count()
        && superblock.block_count <= max_volume_blocks && superblock.bitmap_block_count <= max_bitmap_blocks;
}

ErrorOr<void> Checker::run()
{
    // The checkpoint a mount would use: the valid superblock with the
    // highest generation.
    Vector<u8> buffer;
    TRY(buffer.try_resize(2 * block_size));
    TRY(m_store.read_blocks(0, 2, { buffer.begin(), buffer.size() }));
    Superblock candidates[2];
    bool valid[2];
    for (size_t i = 0; i < 2; ++i)
        valid[i] = read_superblock({ buffer.begin() + i * block_size, block_size }, candidates[i]);
    if (!valid[0] && !valid[1])
        return Error::from_string_literal("no valid superblock");
    size_t slot = !valid[0] || (valid[1] && candidates[1].generation > candidates[0].generation) ? 1 : 0;
    auto const& superblock = candidates[slot];
    if (superblock.generation % 2 != slot)
        problem("superblock of generation {} is in slot {}", superblock.generation, slot);
    m_generation = superblock.generation;
    m_report.generation = superblock.generation;
    m_report.block_count = superblock.block_count;

    TRY(m_referenced.try_resize((superblock.block_count + 63) / 64));
    for (auto& word : m_referenced)
        word = 0;
    reference(0, 2, "the superblocks"_sv);

    Allocator allocator;
    if (auto result = allocator.load(m_store, superblock); result.is_error()) {
        problem("bitmap: {}", result.error());
        return {};
    }
    for (size_t i = 0; i < superblock.bitmap_block_count; ++i)
        reference(superblock.bitmap[i].block, 1, "the bitmap"_sv);

    TRY(check_node(superblock.tree_root, superblock.tree_root_generation, 0xffff, make_key(0, 0, 0), {}, 0));
    check_items();
    check_inodes(superblock);

    // The bitmap must say exactly which blocks are used.
    u64 marked_free = 0, marked_used = 0;
    for (BlockIndex block = 0; block < superblock.block_count; ++block) {
        bool referenced = m_referenced[block / 64] & (1ull << (block % 64));
        bool used = allocator.is_used(block);
        if (referenced && !used) {
            if (marked_free++ < 10)
                problem("block {} is in use but marked free", block);
        } else if (!referenced && used) {
            if (marked_used++ < 10)
                problem("block {} is marked used but nothing uses it (leaked)", block);
        }
        if (referenced)
            ++m_report.used_blocks;
    }
    if (marked_free > 10)
        problem("... {} blocks in use but marked free in all", marked_free);
    if (marked_used > 10)
        problem("... {} blocks leaked in all", marked_used);
    if (m_problem_count > m_report.problems.size())
        problem("... {} problems in all", m_problem_count);
    return {};
}

ErrorOr<void> Checker::check_node(BlockIndex block, u64 generation, u16 level, Key const& low, Optional<Key> const& high, size_t depth)
{
    ++m_report.tree_nodes;
    if (depth > max_depth) {
        problem("tree deeper than {} levels", max_depth);
        return {};
    }
    if (block < 2 || block >= m_report.block_count) {
        problem("tree node at block {} is out of range", block);
        return {};
    }
    reference(block, 1, "a tree node"_sv);
    Vector<u8> buffer;
    TRY(buffer.try_resize(block_size));
    TRY(m_store.read_blocks(block, 1, { buffer.begin(), block_size }));
    NodeHeader header;
    __builtin_memcpy(&header, buffer.begin(), sizeof(header));
    if (header.magic != node_magic) {
        problem("block {} should be a tree node but is not", block);
        return {};
    }
    __builtin_memset(buffer.begin() + __builtin_offsetof(NodeHeader, checksum), 0, sizeof(u32));
    if (crc32c({ buffer.begin(), block_size }) != header.checksum) {
        problem("tree node {} has a bad checksum", block);
        return {};
    }
    if (header.block != block)
        problem("tree node at block {} says it belongs at {}", block, header.block);
    if (header.generation != generation)
        problem("tree node {} has generation {}, its parent expects {}", block, header.generation, generation);
    if (header.generation > m_generation)
        problem("tree node {} is newer ({}) than the checkpoint ({})", block, header.generation, m_generation);
    if (level != 0xffff && header.level != level) {
        problem("tree node {} is at level {}, expected {}", block, header.level, level);
        return {};
    }
    if (level == 0xffff)
        m_report.tree_height = header.level + 1u;

    auto in_range = [&](Key const& key) { return low <= key && (!high.has_value() || key < high.value()); };

    if (header.level == 0) {
        size_t entries_end = sizeof(NodeHeader) + header.count * sizeof(LeafEntry);
        if (entries_end > block_size) {
            problem("leaf {} claims {} entries", block, header.count);
            return {};
        }
        for (size_t i = 0; i < header.count; ++i) {
            LeafEntry entry;
            __builtin_memcpy(&entry, buffer.begin() + sizeof(NodeHeader) + i * sizeof(LeafEntry), sizeof(entry));
            if (entry.value_offset < entries_end || entry.value_offset + entry.value_size > block_size) {
                problem("leaf {} entry {} has its value out of bounds", block, i);
                continue;
            }
            if (!in_range(entry.key))
                problem("leaf {} key ({}, {}, {}) is outside its parent's range", block, entry.key.object, entry.key.type, entry.key.offset);
            if (!m_items.is_empty() && !(m_items.last().key < entry.key))
                problem("key ({}, {}, {}) in leaf {} is out of order", entry.key.object, entry.key.type, entry.key.offset, block);
            Item item { entry.key, {} };
            TRY(item.value.try_resize(entry.value_size));
            __builtin_memcpy(item.value.begin(), buffer.begin() + entry.value_offset, entry.value_size);
            TRY(m_items.try_append(move(item)));
        }
        return {};
    }

    if (header.count == 0 || header.count > max_internal_entries) {
        problem("internal node {} has {} children", block, header.count);
        return {};
    }
    Vector<InternalEntry> entries;
    for (size_t i = 0; i < header.count; ++i) {
        InternalEntry entry;
        __builtin_memcpy(&entry, buffer.begin() + sizeof(NodeHeader) + i * sizeof(InternalEntry), sizeof(entry));
        if (i > 0 && !(entries.last().key < entry.key))
            problem("internal node {} keys are out of order", block);
        if (!in_range(entry.key))
            problem("internal node {} key is outside its parent's range", block);
        if (entry.child_generation > header.generation)
            problem("internal node {} has a child newer than itself", block);
        entries.append(entry);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        Optional<Key> child_high = i + 1 < entries.size() ? Optional<Key>(entries[i + 1].key) : high;
        TRY(check_node(entries[i].child, entries[i].child_generation, static_cast<u16>(header.level - 1), entries[i].key, child_high, depth + 1));
    }
    return {};
}

void Checker::check_items()
{
    // Inodes first, so everything else can refer to them.
    for (auto const& item : m_items) {
        if (item.key.type != item_inode)
            continue;
        if (item.key.offset != 0 || item.value.size() != sizeof(InodeRecord)) {
            problem("inode {} record is malformed", item.key.object);
            continue;
        }
        InodeInfo info;
        __builtin_memcpy(&info.record, item.value.begin(), sizeof(InodeRecord));
        u32 type = info.record.mode & mode_type_mask;
        if (type == mode_directory)
            ++m_report.directories;
        else if (type == mode_regular)
            ++m_report.files;
        else if (type == mode_symbolic_link)
            ++m_report.symbolic_links;
        else
            problem("inode {} has unknown type {:o}", item.key.object, type);
        m_inodes.set(item.key.object, info);
    }

    InodeNumber previous_extent_inode = 0;
    u64 previous_extent_end = 0;
    for (auto const& item : m_items) {
        auto const& key = item.key;
        switch (key.type) {
        case item_inode:
            break;
        case item_directory_entry: {
            auto* directory = m_inodes.get(key.object);
            if (!directory || (directory->record.mode & mode_type_mask) != mode_directory) {
                problem("directory entries under {}, which is not a directory", key.object);
                break;
            }
            for (size_t offset = 0; offset < item.value.size();) {
                DirectoryEntryRecord record;
                if (offset + sizeof(record) > item.value.size()) {
                    problem("directory {} has a truncated entry", key.object);
                    break;
                }
                __builtin_memcpy(&record, item.value.begin() + offset, sizeof(record));
                offset += sizeof(record);
                if (record.name_length == 0 || record.name_length > max_name_length || offset + record.name_length > item.value.size()) {
                    problem("directory {} has an entry with a bad name length", key.object);
                    break;
                }
                StringView name { reinterpret_cast<char const*>(item.value.begin() + offset), record.name_length };
                offset += record.name_length;
                if (name_hash(name.characters_without_null_termination(), name.length()) != key.offset)
                    problem("directory {} entry \"{}\" is filed under the wrong hash", key.object, name);
                if (name == "."_sv || name == ".."_sv || name.contains('/') || name.contains('\0'))
                    problem("directory {} has an entry with an invalid name", key.object);
                ++directory->entries;
                auto* child = m_inodes.get(record.inode);
                if (!child) {
                    problem("directory {} entry \"{}\" names inode {}, which does not exist", key.object, name, record.inode);
                    continue;
                }
                ++child->references;
                if (record.type != directory_type_for_mode(child->record.mode))
                    problem("directory {} entry \"{}\" has the wrong type", key.object, name);
                if ((child->record.mode & mode_type_mask) == mode_directory) {
                    ++m_inodes.get(key.object)->subdirectories;
                    if (child->record.parent != key.object)
                        problem("directory {} is in directory {} but says its parent is {}", record.inode, key.object, child->record.parent);
                }
                auto* list = m_children.get(key.object);
                if (!list) {
                    m_children.set(key.object, Vector<u64> {});
                    list = m_children.get(key.object);
                }
                list->append(record.inode);
            }
            break;
        }
        case item_extent: {
            ++m_report.extents;
            auto* file = m_inodes.get(key.object);
            if (!file || (file->record.mode & mode_type_mask) != mode_regular) {
                problem("extent under {}, which is not a file", key.object);
                break;
            }
            if (item.value.size() != sizeof(ExtentRecord)) {
                problem("file {} has a malformed extent", key.object);
                break;
            }
            ExtentRecord record;
            __builtin_memcpy(&record, item.value.begin(), sizeof(record));
            if (record.count == 0 || record.block < 2 || record.block >= m_report.block_count || record.count > m_report.block_count - record.block) {
                problem("file {} extent at {} points outside the volume", key.object, key.offset);
                break;
            }
            if (previous_extent_inode == key.object && key.offset < previous_extent_end)
                problem("file {} extents overlap at block {}", key.object, key.offset);
            previous_extent_inode = key.object;
            previous_extent_end = key.offset + record.count;
            reference(record.block, record.count, "file data"_sv);
            m_report.data_blocks += record.count;
            file->extent_blocks += record.count;
            file->extent_end = max(file->extent_end, key.offset + record.count);
            break;
        }
        case item_inline_data: {
            auto* link = m_inodes.get(key.object);
            if (!link || (link->record.mode & mode_type_mask) != mode_symbolic_link) {
                problem("inline data under {}, which is not a symbolic link", key.object);
                break;
            }
            link->has_inline = true;
            if (item.value.size() != link->record.size || item.value.is_empty())
                problem("symbolic link {} target length does not match its size", key.object);
            break;
        }
        case item_orphan: {
            ++m_report.orphans;
            auto* orphan = m_inodes.get(key.offset);
            if (key.object != orphan_list_object || !orphan) {
                problem("orphan entry for {}, which does not exist", key.offset);
                break;
            }
            orphan->orphan = true;
            if (orphan->record.link_count != 0)
                problem("orphan {} still has {} links", key.offset, orphan->record.link_count);
            break;
        }
        default:
            problem("item of unknown type {} under {}", key.type, key.object);
        }
    }
}

void Checker::check_inodes(Superblock const& superblock)
{
    auto* root = m_inodes.get(root_directory);
    if (!root || (root->record.mode & mode_type_mask) != mode_directory) {
        problem("the root directory is missing");
        return;
    }
    // Everything with a name must be reachable from the root.
    Vector<u64> queue;
    queue.append(root_directory);
    root->reachable = true;
    for (size_t next = 0; next < queue.size(); ++next) {
        auto* children = m_children.get(queue[next]);
        if (!children)
            continue;
        for (auto child : *children) {
            auto* info = m_inodes.get(child);
            if (info && !info->reachable) {
                info->reachable = true;
                if ((info->record.mode & mode_type_mask) == mode_directory)
                    queue.append(child);
            }
        }
    }

    m_inodes.for_each([&](u64 number, InodeInfo const& info) {
        auto const& record = info.record;
        u32 type = record.mode & mode_type_mask;
        if (number >= superblock.next_object)
            problem("inode {} is not below the next object number {}", number, superblock.next_object);
        if (record.link_count == 0) {
            if (!info.orphan)
                problem("inode {} has no links and is not an orphan (leaked)", number);
            if (info.references != 0)
                problem("inode {} has no links but {} names", number, info.references);
        } else if (!info.reachable) {
            problem("inode {} cannot be reached from the root", number);
        }
        if (type == mode_directory) {
            if (record.link_count != 0) {
                u64 expected = 2 + info.subdirectories;
                if (record.link_count != expected)
                    problem("directory {} has link count {}, expected {}", number, record.link_count, expected);
                if (number != root_directory && info.references != 1)
                    problem("directory {} has {} names, expected 1", number, info.references);
            }
            if (record.size != info.entries)
                problem("directory {} size {} does not match its {} entries", number, record.size, info.entries);
        } else if (record.link_count != 0 && record.link_count != info.references) {
            problem("inode {} has link count {} but {} names", number, record.link_count, info.references);
        }
        if (type == mode_regular) {
            if (record.allocated_blocks != info.extent_blocks)
                problem("file {} says it has {} blocks but its extents have {}", number, record.allocated_blocks, info.extent_blocks);
            if (info.extent_end > (record.size + block_size - 1) / block_size)
                problem("file {} has blocks past its end", number);
        }
        if (type == mode_symbolic_link && !info.has_inline)
            problem("symbolic link {} has no target", number);
    });
}

}

ErrorOr<CheckReport> check_volume(BlockStore& store)
{
    CheckReport report;
    Checker checker(store, report);
    TRY(checker.run());
    return report;
}

}
