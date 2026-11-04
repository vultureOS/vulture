/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/Checksum.h>
#include <LibCompress/Deflate.h>
#include <LibVultureFS/Names.h>
#include <LibVultureFS/Volume.h>

namespace VultureFS {

// File data is written in batches of up to this many blocks.
static constexpr u64 write_batch_blocks = 32;
// Blocks kept back from file data so a checkpoint always has room to
// write the tree.
static constexpr u64 reserved_blocks = 64;
static constexpr u64 max_file_size = 1ull << 44;
static constexpr u32 max_link_count = 65000;
static constexpr size_t keep_tree_nodes = 4096;

static bool is_directory(InodeRecord const& record) { return (record.mode & mode_type_mask) == mode_directory; }
static bool is_symbolic_link(InodeRecord const& record) { return (record.mode & mode_type_mask) == mode_symbolic_link; }
static bool is_regular(InodeRecord const& record) { return (record.mode & mode_type_mask) == mode_regular; }

static ErrorOr<void> validate_name(StringView name)
{
    if (name.is_empty() || name == "."_sv || name == ".."_sv)
        return Error::from_errno(EINVAL);
    if (name.length() > 4 * max_name_length)
        return Error::from_errno(ENAMETOOLONG);
    for (char c : name) {
        if (c == '/' || c == '\0')
            return Error::from_errno(EINVAL);
    }
    if (!is_valid_name_encoding(name))
        return Error::from_errno(EILSEQ);
    if (name_length_in_utf16_units(name) > max_name_length)
        return Error::from_errno(ENAMETOOLONG);
    return {};
}

static ReadonlyBytes bytes_of(InodeRecord const& record) { return { reinterpret_cast<u8 const*>(&record), sizeof(record) }; }

// Marks the volume failed if `result` is an error: used once an operation
// has started changing the tree, which it could leave half-changed.
#define CHANGE(expression)                        \
    ({                                            \
        auto&& change_result = (expression);      \
        if (change_result.is_error()) {           \
            m_failed = true;                      \
            return change_result.release_error(); \
        }                                         \
        change_result.release_value();            \
    })

Volume::Volume(BlockStore& store, Clock clock)
    : m_store(store)
    , m_clock(clock)
    , m_tree(store, m_allocator)
{
}

ErrorOr<NonnullOwnPtr<Volume>> Volume::format(BlockStore& store, StringView name, ReadonlyBytes uuid, Clock clock, FormatOptions options)
{
    auto* raw = new (Base::nothrow) Volume(store, clock);
    if (!raw)
        return Error::from_errno(ENOMEM);
    auto volume = adopt_own(*raw);
    u64 blocks = min(store.block_count(), max_volume_blocks);
    TRY(volume->m_allocator.initialize(blocks));
    TRY(volume->m_tree.create_empty());

    auto& superblock = volume->m_superblock;
    __builtin_memcpy(superblock.magic, superblock_magic, sizeof(superblock.magic));
    superblock.version = format_version;
    superblock.block_count = blocks;
    superblock.next_object = first_free_object;
    superblock.next_event = 1;
    superblock.oldest_event = 1;
    superblock.next_snapshot = 1;
    superblock.created_time = volume->now();
    superblock.role = options.role;
    superblock.features_required = options.case_sensitive ? 0 : feature_case_insensitive;
    superblock.features_read_only = options.sealed ? feature_sealed : 0;
    if (options.group_uuid)
        __builtin_memcpy(superblock.group_uuid, options.group_uuid, sizeof(superblock.group_uuid));
    __builtin_memcpy(superblock.uuid, uuid.data(), min(uuid.size(), sizeof(superblock.uuid)));
    __builtin_memcpy(superblock.name, name.characters_without_null_termination(), min(name.length(), sizeof(superblock.name) - 1));

    // An older volume's superblocks must not outlive this one.
    Vector<u8> zeros;
    TRY(zeros.try_resize(2 * block_size));
    __builtin_memset(zeros.begin(), 0, zeros.size());
    TRY(store.write_blocks(0, 2, { zeros.begin(), zeros.size() }));

    InodeRecord root {};
    root.mode = mode_directory | 0755;
    root.link_count = 2;
    root.access_time = root.modification_time = root.change_time = root.birth_time = superblock.created_time;
    root.parent = root_directory;
    TRY(volume->put_inode(root_directory, root));
    volume->m_dirty = true;
    TRY(volume->checkpoint());
    return volume;
}

// Whether this is a block that could be a superblock of this format. It says if it's only that of an old one.
static bool read_superblock(u8 const* block, u64 store_blocks, Superblock& superblock, bool& is_old)
{
    __builtin_memcpy(&superblock, block, sizeof(superblock));
    if (__builtin_memcmp(superblock.magic, old_superblock_magic, sizeof(superblock.magic)) == 0) {
        is_old = true;
        return false;
    }
    if (__builtin_memcmp(superblock.magic, superblock_magic, sizeof(superblock.magic)) != 0)
        return false;
    CRC32C crc;
    crc.update({ block, __builtin_offsetof(Superblock, checksum) });
    u8 const zero[4] {};
    crc.update(zero);
    crc.update({ block + __builtin_offsetof(Superblock, checksum) + 4, block_size - __builtin_offsetof(Superblock, checksum) - 4 });
    if (crc.digest() != superblock.checksum)
        return false;
    return superblock.version == format_version && superblock.generation >= 1 && superblock.block_count <= store_blocks
        && superblock.block_count <= max_volume_blocks;
}

ErrorOr<NonnullOwnPtr<Volume>> Volume::mount(BlockStore& store, Clock clock, MountOptions options)
{
    Vector<u8> buffer;
    TRY(buffer.try_resize(2 * block_size));
    TRY(store.read_blocks(0, 2, { buffer.begin(), buffer.size() }));
    Superblock candidates[2];
    bool valid[2];
    bool is_old = false;
    for (size_t i = 0; i < 2; ++i)
        valid[i] = read_superblock(buffer.begin() + i * block_size, store.block_count(), candidates[i], is_old);
    if (!valid[0] && !valid[1]) {
        if (is_old)
            return Error::from_string_literal("vultureFS: this volume is in version 1 of the format, which this system doesn't read: make the disk anew");
        return Error::from_string_literal("vultureFS: no valid superblock");
    }

    // The newest checkpoint first; the one before it if that is damaged.
    size_t order[2] = { 0, 1 };
    if (!valid[0] || (valid[1] && candidates[1].generation > candidates[0].generation)) {
        order[0] = 1;
        order[1] = 0;
    }
    Error last_error = Error::from_errno(EIO);
    for (size_t slot : order) {
        if (!valid[slot])
            continue;
        auto const& superblock = candidates[slot];
        if (superblock.features_required & ~known_features_required)
            return Error::from_string_literal("vultureFS: this volume needs something this system doesn't have");
        auto* raw = new (Base::nothrow) Volume(store, clock);
        if (!raw)
            return Error::from_errno(ENOMEM);
        auto volume = adopt_own(*raw);
        volume->m_read_only = options.read_only || (superblock.features_read_only != 0);
        auto result = volume->mount_checkpoint(superblock);
        if (result.is_error()) {
            last_error = result.release_error();
            continue;
        }
        if (!volume->m_read_only) {
            if (superblock.pending_rollback != 0)
                TRY(volume->go_back(superblock.pending_rollback));
            TRY(volume->delete_orphans());
        }
        return volume;
    }
    return last_error;
}

ErrorOr<NonnullOwnPtr<Volume>> Volume::mount_snapshot(BlockStore& store, Clock clock, StringView name)
{
    auto current = TRY(Volume::mount(store, clock, { .read_only = true }));
    auto id = TRY(current->find_snapshot(name));
    if (!id.has_value())
        return Error::from_errno(ENOENT);
    auto saved = TRY(current->saved_superblock(id.value()));
    if (!saved.has_value())
        return Error::from_errno(EIO);
    auto* raw = new (Base::nothrow) Volume(store, clock);
    if (!raw)
        return Error::from_errno(ENOMEM);
    auto volume = adopt_own(*raw);
    volume->m_read_only = true;
    TRY(volume->mount_checkpoint(saved.value()));
    return volume;
}

ErrorOr<void> Volume::mount_checkpoint(Superblock const& superblock)
{
    m_superblock = superblock;
    // (Nothing's going to be allocated on a volume that can't change, so its bitmap isn't worth reading.)
    if (m_read_only) {
        TRY(m_allocator.initialize(superblock.block_count));
        TRY(m_tree.load(superblock.tree_root, superblock.tree_root_generation));
        return {};
    }
    TRY(m_allocator.load(m_store, superblock));
    TRY(m_tree.load(superblock.tree_root, superblock.tree_root_generation));
    TRY(load_snapshots());
    return {};
}

ErrorOr<Optional<Superblock>> Volume::saved_superblock(u64 snapshot)
{
    auto value = TRY(m_tree.find(make_key(snapshots_object, item_snapshot, snapshot)));
    if (!value.has_value() || value->size() < sizeof(SnapshotRecord))
        return Optional<Superblock> {};
    SnapshotRecord record;
    __builtin_memcpy(&record, value->begin(), sizeof(record));
    if (record.saved_superblock < 2 || record.saved_superblock >= m_superblock.block_count)
        return Error::from_string_literal("vultureFS: a snapshot's superblock is out of range");
    Vector<u8> block;
    TRY(block.try_resize(block_size));
    TRY(m_store.read_blocks(record.saved_superblock, 1, { block.begin(), block_size }));
    if (crc32c({ block.begin(), block_size }) != record.saved_checksum)
        return Error::from_string_literal("vultureFS: a snapshot's superblock checksum mismatch");
    Superblock saved;
    __builtin_memcpy(&saved, block.begin(), sizeof(saved));
    return Optional<Superblock>(saved);
}

ErrorOr<void> Volume::load_snapshots()
{
    Vector<u64> ids;
    TRY(m_tree.for_each_from(make_key(snapshots_object, item_snapshot, 0), [&](Key const& key, ReadonlyBytes) {
        if (key.object != snapshots_object || key.type != item_snapshot)
            return Iteration::Stop;
        ids.append(key.offset);
        return Iteration::Continue;
    }));
    TRY(m_allocator.clear_snapshots());
    for (auto id : ids) {
        auto saved = TRY(saved_superblock(id));
        if (!saved.has_value())
            return Error::from_string_literal("vultureFS: a snapshot has gone missing");
        TRY(m_allocator.add_snapshot(m_store, saved.value()));
    }
    return {};
}

ErrorOr<void> Volume::delete_orphans()
{
    Vector<InodeNumber> orphans;
    TRY(m_tree.for_each_from(make_key(orphan_list_object, item_orphan, 0), [&](Key const& key, ReadonlyBytes) {
        if (key.object != orphan_list_object || key.type != item_orphan)
            return Iteration::Stop;
        orphans.append(key.offset);
        return Iteration::Continue;
    }));
    if (orphans.is_empty())
        return {};
    for (auto inode : orphans)
        TRY(release(inode));
    return checkpoint();
}

Statistics Volume::statistics() const
{
    if (m_read_only)
        return { m_superblock.generation, m_superblock.block_count, m_superblock.free_blocks, m_tree.height(), 0 };
    return { m_superblock.generation, m_allocator.block_count(), m_allocator.free_count(), m_tree.height(), m_allocator.held_count() };
}

StringView Volume::name() const
{
    size_t length = 0;
    while (length < sizeof(m_superblock.name) && m_superblock.name[length])
        ++length;
    return { m_superblock.name, length };
}

ErrorOr<void> Volume::mutate()
{
    if (m_read_only)
        return Error::from_errno(EROFS);
    if (m_failed)
        return Error::from_errno(EIO);
    m_dirty = true;
    return {};
}

u64 Volume::blocks_to_keep_back() const
{
    return reserved_blocks + m_allocator.block_count() / blocks_per_bitmap_block + m_allocator.block_count() / (blocks_per_bitmap_block * bitmap_pointers_per_index_block) + 2;
}

ErrorOr<InodeRecord> Volume::inode(InodeNumber number)
{
    auto value = TRY(m_tree.find(make_key(number, item_inode, 0)));
    if (!value.has_value())
        return Error::from_errno(ENOENT);
    if (value->size() != sizeof(InodeRecord))
        return Error::from_string_literal("vultureFS: inode record of the wrong size");
    InodeRecord record;
    __builtin_memcpy(&record, value->begin(), sizeof(record));
    return record;
}

ErrorOr<void> Volume::put_inode(InodeNumber number, InodeRecord const& record)
{
    return m_tree.insert(make_key(number, item_inode, 0), bytes_of(record));
}

ErrorOr<InodeRecord> Volume::directory(InodeNumber number)
{
    auto record = TRY(inode(number));
    if (!is_directory(record))
        return Error::from_errno(ENOTDIR);
    return record;
}

ErrorOr<Key> Volume::entry_key(InodeNumber directory_inode, StringView name, Vector<u8>& folded)
{
    TRY(fold_name(name, is_case_insensitive(), folded));
    return make_key(directory_inode, item_directory_entry, name_hash(reinterpret_cast<char const*>(folded.begin()), folded.size()));
}

ErrorOr<Optional<DirectoryEntry>> Volume::find_entry(InodeNumber directory_inode, StringView name, Vector<u8>* spelling)
{
    Vector<u8> folded;
    auto key = TRY(entry_key(directory_inode, name, folded));
    auto value = TRY(m_tree.find(key));
    if (!value.has_value())
        return Optional<DirectoryEntry> {};
    auto const& bytes = value.value();
    Vector<u8> other;
    for (size_t offset = 0; offset + sizeof(DirectoryEntryRecord) <= bytes.size();) {
        DirectoryEntryRecord record;
        __builtin_memcpy(&record, bytes.begin() + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.name_length > bytes.size())
            break;
        StringView stored { reinterpret_cast<char const*>(bytes.begin() + offset), record.name_length };
        offset += record.name_length;
        TRY(fold_name(stored, is_case_insensitive(), other));
        if (other.size() == folded.size() && __builtin_memcmp(other.begin(), folded.begin(), folded.size()) == 0) {
            if (spelling) {
                TRY(spelling->try_resize(stored.length()));
                __builtin_memcpy(spelling->begin(), stored.characters_without_null_termination(), stored.length());
            }
            return Optional<DirectoryEntry>(DirectoryEntry { record.inode, record.type });
        }
    }
    return Optional<DirectoryEntry> {};
}

ErrorOr<void> Volume::add_entry(InodeNumber directory_inode, StringView name, DirectoryEntry entry)
{
    Vector<u8> folded;
    auto key = TRY(entry_key(directory_inode, name, folded));
    auto existing = TRY(m_tree.find(key));
    Vector<u8> value;
    if (existing.has_value())
        value = existing.release_value();
    size_t offset = value.size();
    if (offset + sizeof(DirectoryEntryRecord) + name.length() > max_value_size)
        return Error::from_errno(ENOSPC);
    TRY(value.try_resize(offset + sizeof(DirectoryEntryRecord) + name.length()));
    DirectoryEntryRecord record { entry.inode, entry.type, 0, static_cast<u16>(name.length()) };
    __builtin_memcpy(value.begin() + offset, &record, sizeof(record));
    __builtin_memcpy(value.begin() + offset + sizeof(record), name.characters_without_null_termination(), name.length());
    return m_tree.insert(key, { value.begin(), value.size() });
}

ErrorOr<void> Volume::remove_entry(InodeNumber directory_inode, StringView name)
{
    Vector<u8> folded;
    auto key = TRY(entry_key(directory_inode, name, folded));
    auto existing = TRY(m_tree.find(key));
    if (!existing.has_value())
        return Error::from_errno(ENOENT);
    auto const& bytes = existing.value();
    Vector<u8> kept;
    Vector<u8> other;
    bool found = false;
    for (size_t offset = 0; offset + sizeof(DirectoryEntryRecord) <= bytes.size();) {
        DirectoryEntryRecord record;
        __builtin_memcpy(&record, bytes.begin() + offset, sizeof(record));
        size_t length = sizeof(record) + record.name_length;
        if (offset + length > bytes.size())
            break;
        StringView stored { reinterpret_cast<char const*>(bytes.begin() + offset + sizeof(record)), record.name_length };
        TRY(fold_name(stored, is_case_insensitive(), other));
        if (!found && other.size() == folded.size() && __builtin_memcmp(other.begin(), folded.begin(), folded.size()) == 0) {
            found = true;
        } else {
            for (size_t i = 0; i < length; ++i)
                TRY(kept.try_append(bytes[offset + i]));
        }
        offset += length;
    }
    if (!found)
        return Error::from_errno(ENOENT);
    if (kept.is_empty()) {
        (void)TRY(m_tree.remove(key));
        return {};
    }
    return m_tree.insert(key, { kept.begin(), kept.size() });
}

ErrorOr<bool> Volume::is_empty_directory(InodeNumber number)
{
    bool empty = true;
    TRY(for_each_entry(number, [&](StringView, DirectoryEntry) {
        empty = false;
        return Iteration::Stop;
    }));
    return empty;
}

ErrorOr<DirectoryEntry> Volume::lookup(InodeNumber directory_inode, StringView name)
{
    auto record = TRY(directory(directory_inode));
    if (name == "."_sv)
        return DirectoryEntry { directory_inode, type_directory };
    if (name == ".."_sv)
        return DirectoryEntry { record.parent, type_directory };
    // Something that couldn't have been made is simply not there. Too long, on the other hand, is said.
    if (!is_valid_name_encoding(name))
        return Error::from_errno(ENOENT);
    if (name_length_in_utf16_units(name) > max_name_length)
        return Error::from_errno(ENAMETOOLONG);
    auto entry = TRY(find_entry(directory_inode, name));
    if (!entry.has_value())
        return Error::from_errno(ENOENT);
    return entry.value();
}

ErrorOr<InodeNumber> Volume::create(InodeNumber directory_inode, StringView name, u32 mode, u32 uid, u32 gid, StringView link_target)
{
    TRY(validate_name(name));
    auto parent = TRY(directory(directory_inode));
    if (TRY(find_entry(directory_inode, name)).has_value())
        return Error::from_errno(EEXIST);
    u32 type = mode & mode_type_mask;
    if (type == 0)
        type = mode_regular;
    if (type != mode_regular && type != mode_directory && type != mode_symbolic_link && type != mode_fifo && type != mode_socket)
        return Error::from_errno(ENOTSUP);
    if (type == mode_symbolic_link && (link_target.is_empty() || link_target.length() > max_link_target))
        return Error::from_errno(link_target.is_empty() ? ENOENT : ENAMETOOLONG);
    if (type == mode_directory && parent.link_count >= max_link_count)
        return Error::from_errno(EMLINK);
    TRY(mutate());

    InodeNumber number = m_superblock.next_object++;
    i64 time = now();
    InodeRecord record {};
    record.mode = type | (mode & 07777);
    record.link_count = type == mode_directory ? 2 : 1;
    record.uid = uid;
    record.gid = gid;
    record.access_time = record.modification_time = record.change_time = record.birth_time = time;
    record.parent = type == mode_directory ? directory_inode : 0;
    if (type == mode_symbolic_link) {
        record.size = link_target.length();
        CHANGE(m_tree.insert(make_key(number, item_inline_data, 0), link_target.bytes()));
    }
    CHANGE(put_inode(number, record));
    CHANGE(add_entry(directory_inode, name, { number, directory_type_for_mode(record.mode) }));
    CHANGE(adjust_directory(directory_inode, 1, type == mode_directory ? 1 : 0));
    return number;
}

ErrorOr<void> Volume::adjust_directory(InodeNumber number, i64 entries, i64 links)
{
    auto record = TRY(directory(number));
    record.size = static_cast<u64>(static_cast<i64>(record.size) + entries);
    record.link_count = static_cast<u32>(static_cast<i64>(record.link_count) + links);
    record.modification_time = record.change_time = now();
    return put_inode(number, record);
}

ErrorOr<void> Volume::link(InodeNumber directory_inode, StringView name, InodeNumber target)
{
    TRY(validate_name(name));
    (void)TRY(directory(directory_inode));
    auto record = TRY(inode(target));
    if (is_directory(record))
        return Error::from_errno(EPERM);
    if (record.link_count == 0)
        return Error::from_errno(ENOENT);
    if (record.link_count >= max_link_count)
        return Error::from_errno(EMLINK);
    if (TRY(find_entry(directory_inode, name)).has_value())
        return Error::from_errno(EEXIST);
    TRY(mutate());
    ++record.link_count;
    record.change_time = now();
    CHANGE(put_inode(target, record));
    CHANGE(add_entry(directory_inode, name, { target, directory_type_for_mode(record.mode) }));
    CHANGE(adjust_directory(directory_inode, 1, 0));
    return {};
}

// Removes the name, which must exist, and one link of `record`; with none
// left the inode becomes an orphan until release().
ErrorOr<Unlinked> Volume::drop_link(InodeNumber directory_inode, StringView name, InodeNumber number, InodeRecord& record)
{
    CHANGE(remove_entry(directory_inode, name));
    if (is_directory(record)) {
        record.link_count = 0;
        CHANGE(adjust_directory(directory_inode, -1, -1));
    } else {
        --record.link_count;
        CHANGE(adjust_directory(directory_inode, -1, 0));
    }
    record.change_time = now();
    if (record.link_count == 0)
        CHANGE(m_tree.insert(make_key(orphan_list_object, item_orphan, number), {}));
    CHANGE(put_inode(number, record));
    return Unlinked { number, record.link_count == 0 };
}

ErrorOr<Unlinked> Volume::unlink(InodeNumber directory_inode, StringView name)
{
    TRY(validate_name(name));
    (void)TRY(directory(directory_inode));
    auto entry = TRY(find_entry(directory_inode, name));
    if (!entry.has_value())
        return Error::from_errno(ENOENT);
    auto record = TRY(inode(entry->inode));
    if (is_directory(record))
        return Error::from_errno(EISDIR);
    TRY(mutate());
    return drop_link(directory_inode, name, entry->inode, record);
}

ErrorOr<Unlinked> Volume::remove_directory(InodeNumber directory_inode, StringView name)
{
    if (name == ".."_sv)
        return Error::from_errno(ENOTEMPTY);
    TRY(validate_name(name));
    (void)TRY(directory(directory_inode));
    auto entry = TRY(find_entry(directory_inode, name));
    if (!entry.has_value())
        return Error::from_errno(ENOENT);
    auto record = TRY(inode(entry->inode));
    if (!is_directory(record))
        return Error::from_errno(ENOTDIR);
    if (!TRY(is_empty_directory(entry->inode)))
        return Error::from_errno(ENOTEMPTY);
    TRY(mutate());
    return drop_link(directory_inode, name, entry->inode, record);
}

ErrorOr<bool> Volume::is_ancestor(InodeNumber ancestor, InodeNumber number)
{
    for (size_t depth = 0; depth < 4096; ++depth) {
        if (number == ancestor)
            return true;
        if (number == root_directory)
            return false;
        number = TRY(directory(number)).parent;
    }
    return Error::from_string_literal("vultureFS: directory parents form a loop");
}

ErrorOr<Optional<Unlinked>> Volume::rename(InodeNumber old_directory, StringView old_name, InodeNumber new_directory, StringView new_name)
{
    TRY(validate_name(old_name));
    TRY(validate_name(new_name));
    (void)TRY(directory(old_directory));
    (void)TRY(directory(new_directory));
    auto entry = TRY(find_entry(old_directory, old_name));
    if (!entry.has_value())
        return Error::from_errno(ENOENT);
    auto record = TRY(inode(entry->inode));
    bool moving_directory = is_directory(record);
    // A directory cannot move into itself or below.
    if (moving_directory && old_directory != new_directory && TRY(is_ancestor(entry->inode, new_directory)))
        return Error::from_errno(EINVAL);

    Vector<u8> existing_spelling;
    auto existing = TRY(find_entry(new_directory, new_name, &existing_spelling));
    Optional<InodeRecord> replaced_record;
    if (existing.has_value()) {
        if (existing->inode == entry->inode) {
            // Its own name, spelled another way, is how something's name gets changed to that.
            Vector<u8> old_folded;
            Vector<u8> new_folded;
            TRY(fold_name(old_name, is_case_insensitive(), old_folded));
            TRY(fold_name(new_name, is_case_insensitive(), new_folded));
            bool const same_name = old_directory == new_directory && old_folded.size() == new_folded.size()
                && __builtin_memcmp(old_folded.begin(), new_folded.begin(), new_folded.size()) == 0;
            if (!same_name)
                return Optional<Unlinked> {};
            TRY(mutate());
            CHANGE(remove_entry(old_directory, old_name));
            CHANGE(add_entry(new_directory, new_name, entry.value()));
            record.change_time = now();
            CHANGE(put_inode(entry->inode, record));
            return Optional<Unlinked> {};
        }
        auto replaced = TRY(inode(existing->inode));
        if (moving_directory && !is_directory(replaced))
            return Error::from_errno(ENOTDIR);
        if (!moving_directory && is_directory(replaced))
            return Error::from_errno(EISDIR);
        if (is_directory(replaced) && !TRY(is_empty_directory(existing->inode)))
            return Error::from_errno(ENOTEMPTY);
        replaced_record = replaced;
    }
    if (moving_directory && old_directory != new_directory && !existing.has_value()) {
        if (TRY(directory(new_directory)).link_count >= max_link_count)
            return Error::from_errno(EMLINK);
    }
    TRY(mutate());

    Optional<Unlinked> result;
    if (existing.has_value())
        result = CHANGE(drop_link(new_directory, new_name, existing->inode, replaced_record.value()));
    CHANGE(remove_entry(old_directory, old_name));
    // In place of something, it takes on that thing's spelling too, as on a Mac.
    StringView const spelled = existing.has_value() ? StringView { reinterpret_cast<char const*>(existing_spelling.begin()), existing_spelling.size() } : new_name;
    CHANGE(add_entry(new_directory, spelled, entry.value()));
    i64 links = moving_directory && old_directory != new_directory ? 1 : 0;
    CHANGE(adjust_directory(old_directory, -1, -links));
    CHANGE(adjust_directory(new_directory, 1, links));
    if (moving_directory)
        record.parent = new_directory;
    record.change_time = now();
    CHANGE(put_inode(entry->inode, record));
    return result;
}

ErrorOr<void> Volume::release_extras(InodeNumber number)
{
    Vector<InodeNumber> streams;
    Vector<u64> xattr_keys;
    TRY(m_tree.for_each_from(make_key(number, item_xattr, 0), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != number || key.type != item_xattr)
            return Iteration::Stop;
        xattr_keys.append(key.offset);
        for (size_t offset = 0; offset + sizeof(XattrRecord) <= value.size();) {
            XattrRecord record;
            __builtin_memcpy(&record, value.data() + offset, sizeof(record));
            if (record.flags & xattr_in_stream)
                streams.append(record.stream_inode);
            offset += sizeof(record) + record.name_length + record.inline_size;
        }
        return Iteration::Continue;
    }));
    for (auto key : xattr_keys)
        (void)TRY(m_tree.remove(make_key(number, item_xattr, key)));
    for (auto stream : streams) {
        (void)TRY(punch(stream, 0, ~0ull));
        (void)TRY(m_tree.remove(make_key(stream, item_inode, 0)));
    }
    (void)TRY(m_tree.remove(make_key(number, item_acl, 0)));
    return {};
}

ErrorOr<void> Volume::release(InodeNumber number)
{
    auto record = TRY(inode(number));
    if (record.link_count != 0)
        return {};
    TRY(mutate());
    if (m_chunk_inode == number)
        forget_chunk();
    (void)CHANGE(punch(number, 0, ~0ull));
    CHANGE(release_extras(number));
    (void)CHANGE(m_tree.remove(make_key(number, item_inline_data, 0)));
    (void)CHANGE(m_tree.remove(make_key(number, item_inode, 0)));
    (void)CHANGE(m_tree.remove(make_key(orphan_list_object, item_orphan, number)));
    return {};
}

static ExtentRecord record_of_extent(u64 block, u64 count, u64 logical, u8 compression, u32 compressed_size)
{
    ExtentRecord record {};
    record.block = block;
    record.count = count;
    record.logical_count = compression == compression_none ? 0 : static_cast<u32>(logical);
    record.compression = compression;
    record.compressed_size = compressed_size;
    return record;
}

ErrorOr<void> Volume::put_extent(InodeNumber number, Extent const& extent)
{
    auto const record = record_of_extent(extent.block, extent.count, extent.logical, extent.compression, extent.compressed_size);
    return m_tree.insert(make_key(number, item_extent, extent.file_block), { reinterpret_cast<u8 const*>(&record), sizeof(record) });
}

ErrorOr<Optional<Volume::Extent>> Volume::find_extent(InodeNumber number, u64 file_block)
{
    auto item = TRY(m_tree.find_at_or_before(make_key(number, item_extent, file_block)));
    if (!item.has_value() || item->key.object != number || item->key.type != item_extent)
        return Optional<Extent> {};
    if (item->value.size() != sizeof(ExtentRecord))
        return Error::from_string_literal("vultureFS: extent record of the wrong size");
    ExtentRecord record;
    __builtin_memcpy(&record, item->value.begin(), sizeof(record));
    u64 const logical = record.logical_count ? record.logical_count : record.count;
    if (file_block - item->key.offset >= logical)
        return Optional<Extent> {};
    return Optional<Extent>(Extent { item->key.offset, record.block, record.count, logical, record.compression, record.compressed_size });
}

ErrorOr<Vector<Volume::Extent>> Volume::extents_in(InodeNumber number, u64 first, u64 end)
{
    Vector<Extent> extents;
    auto covering = TRY(find_extent(number, first));
    if (covering.has_value() && covering->file_block < first)
        extents.append(covering.value());
    bool corrupt = false;
    TRY(m_tree.for_each_from(make_key(number, item_extent, first), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != number || key.type != item_extent || key.offset >= end)
            return Iteration::Stop;
        if (value.size() != sizeof(ExtentRecord)) {
            corrupt = true;
            return Iteration::Stop;
        }
        ExtentRecord record;
        __builtin_memcpy(&record, value.data(), sizeof(record));
        u64 const logical = record.logical_count ? record.logical_count : record.count;
        extents.append(Extent { key.offset, record.block, record.count, logical, record.compression, record.compressed_size });
        return Iteration::Continue;
    }));
    if (corrupt)
        return Error::from_string_literal("vultureFS: extent record of the wrong size");
    return extents;
}

ErrorOr<u64> Volume::punch(InodeNumber number, u64 first, u64 end)
{
    auto extents = TRY(extents_in(number, first, end));
    u64 freed = 0;
    for (auto const& extent : extents) {
        u64 extent_end = extent.file_block + extent.logical;
        if (extent.compression != compression_none) {
            // These are written and let go of whole: anything else is a mistake in here.
            if (extent.file_block < first || extent_end > end)
                return Error::from_string_literal("vultureFS: a compressed extent would have to be split");
            TRY(m_tree.remove(make_key(number, item_extent, extent.file_block)));
            TRY(drop_references(extent.block, extent.count));
            freed += extent.count;
            continue;
        }
        TRY(m_tree.remove(make_key(number, item_extent, extent.file_block)));
        u64 low = max(extent.file_block, first);
        u64 high = min(extent_end, end);
        TRY(drop_references(extent.block + (low - extent.file_block), high - low));
        freed += high - low;
        if (extent.file_block < low)
            TRY(put_extent(number, Extent { extent.file_block, extent.block, low - extent.file_block, low - extent.file_block, compression_none, 0 }));
        if (high < extent_end)
            TRY(put_extent(number, Extent { high, extent.block + (high - extent.file_block), extent_end - high, extent_end - high, compression_none, 0 }));
    }
    return freed;
}

ErrorOr<void> Volume::add_extent(InodeNumber number, Extent extent)
{
    // Grow the extent just before when this continues it on disk too.
    if (extent.file_block > 0 && extent.compression == compression_none) {
        auto previous = TRY(find_extent(number, extent.file_block - 1));
        if (previous.has_value() && previous->compression == compression_none && previous->block + previous->count == extent.block) {
            Extent merged = previous.value();
            merged.count += extent.count;
            merged.logical = merged.count;
            return put_extent(number, merged);
        }
    }
    return put_extent(number, extent);
}

ErrorOr<Optional<BlockIndex>> Volume::map_block(InodeNumber number, u64 file_block)
{
    auto extent = TRY(find_extent(number, file_block));
    if (!extent.has_value() || extent->compression != compression_none)
        return Optional<BlockIndex> {};
    return Optional<BlockIndex>(extent->block + (file_block - extent->file_block));
}

// ---- Who has which blocks.

ErrorOr<void> Volume::add_references(BlockIndex first, u64 count)
{
    u64 const end = first + count;
    struct Run {
        BlockIndex first;
        u64 count;
        u64 references;
    };
    Vector<Run> runs;
    auto before = TRY(m_tree.find_at_or_before(make_key(shared_blocks_object, item_shared_blocks, first)));
    if (before.has_value() && before->key.object == shared_blocks_object && before->key.type == item_shared_blocks) {
        SharedBlocksRecord record;
        __builtin_memcpy(&record, before->value.begin(), sizeof(record));
        if (before->key.offset + record.count > first && before->key.offset < first)
            runs.append({ before->key.offset, record.count, record.references });
    }
    TRY(m_tree.for_each_from(make_key(shared_blocks_object, item_shared_blocks, first), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != shared_blocks_object || key.type != item_shared_blocks || key.offset >= end)
            return Iteration::Stop;
        SharedBlocksRecord record;
        __builtin_memcpy(&record, value.data(), sizeof(record));
        runs.append({ key.offset, record.count, record.references });
        return Iteration::Continue;
    }));
    auto put = [&](BlockIndex at, u64 how_many, u64 references) -> ErrorOr<void> {
        SharedBlocksRecord record { how_many, references };
        return m_tree.insert(make_key(shared_blocks_object, item_shared_blocks, at), { reinterpret_cast<u8 const*>(&record), sizeof(record) });
    };
    BlockIndex position = first;
    for (auto const& run : runs) {
        u64 const run_end = run.first + run.count;
        TRY(m_tree.remove(make_key(shared_blocks_object, item_shared_blocks, run.first)));
        // What's before this was held by one.
        if (position < run.first)
            TRY(put(position, run.first - position, 2));
        if (run.first < first)
            TRY(put(run.first, first - run.first, run.references));
        u64 const low = max(run.first, first);
        u64 const high = min(run_end, end);
        TRY(put(low, high - low, run.references + 1));
        if (high < run_end)
            TRY(put(high, run_end - high, run.references));
        position = high;
    }
    if (position < end)
        TRY(put(position, end - position, 2));
    return {};
}

ErrorOr<void> Volume::drop_references(BlockIndex first, u64 count)
{
    u64 const end = first + count;
    struct Run {
        BlockIndex first;
        u64 count;
        u64 references;
    };
    Vector<Run> runs;
    auto before = TRY(m_tree.find_at_or_before(make_key(shared_blocks_object, item_shared_blocks, first)));
    if (before.has_value() && before->key.object == shared_blocks_object && before->key.type == item_shared_blocks) {
        SharedBlocksRecord record;
        __builtin_memcpy(&record, before->value.begin(), sizeof(record));
        if (before->key.offset + record.count > first && before->key.offset < first)
            runs.append({ before->key.offset, record.count, record.references });
    }
    TRY(m_tree.for_each_from(make_key(shared_blocks_object, item_shared_blocks, first), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != shared_blocks_object || key.type != item_shared_blocks || key.offset >= end)
            return Iteration::Stop;
        SharedBlocksRecord record;
        __builtin_memcpy(&record, value.data(), sizeof(record));
        runs.append({ key.offset, record.count, record.references });
        return Iteration::Continue;
    }));
    auto put = [&](BlockIndex at, u64 how_many, u64 references) -> ErrorOr<void> {
        SharedBlocksRecord record { how_many, references };
        return m_tree.insert(make_key(shared_blocks_object, item_shared_blocks, at), { reinterpret_cast<u8 const*>(&record), sizeof(record) });
    };
    auto let_go = [&](BlockIndex at, u64 how_many) -> ErrorOr<void> {
        m_allocator.free(at, how_many);
        return clear_checksums(at, how_many);
    };
    BlockIndex position = first;
    for (auto const& run : runs) {
        u64 const run_end = run.first + run.count;
        TRY(m_tree.remove(make_key(shared_blocks_object, item_shared_blocks, run.first)));
        // What's before this had only the one, which is going.
        if (position < run.first)
            TRY(let_go(position, run.first - position));
        if (run.first < first)
            TRY(put(run.first, first - run.first, run.references));
        u64 const low = max(run.first, first);
        u64 const high = min(run_end, end);
        // Down to one, it's no longer anything to keep a note of.
        if (run.references > 2)
            TRY(put(low, high - low, run.references - 1));
        if (high < run_end)
            TRY(put(high, run_end - high, run.references));
        position = high;
    }
    if (position < end)
        TRY(let_go(position, end - position));
    return {};
}

static u32 block_checksum(u8 const* data)
{
    u32 const crc = crc32c({ data, block_size });
    return crc ? crc : 1;
}

ErrorOr<void> Volume::set_checksums(BlockIndex first, u64 count, u8 const* data)
{
    for (u64 done = 0; done < count;) {
        BlockIndex const block = first + done;
        BlockIndex const group = block - block % checksums_per_item;
        u64 const here = min(count - done, group + checksums_per_item - block);
        auto const key = make_key(checksums_object, item_checksums, group);
        auto existing = TRY(m_tree.find(key));
        u32 values[checksums_per_item];
        if (existing.has_value() && existing->size() == sizeof(values))
            __builtin_memcpy(values, existing->begin(), sizeof(values));
        else
            __builtin_memset(values, 0, sizeof(values));
        for (u64 i = 0; i < here; ++i)
            values[block - group + i] = block_checksum(data + (done + i) * block_size);
        TRY(m_tree.insert(key, { reinterpret_cast<u8 const*>(values), sizeof(values) }));
        done += here;
    }
    return {};
}

ErrorOr<void> Volume::clear_checksums(BlockIndex first, u64 count)
{
    for (u64 done = 0; done < count;) {
        BlockIndex const block = first + done;
        BlockIndex const group = block - block % checksums_per_item;
        u64 const here = min(count - done, group + checksums_per_item - block);
        auto const key = make_key(checksums_object, item_checksums, group);
        auto existing = TRY(m_tree.find(key));
        if (existing.has_value() && existing->size() == checksums_per_item * sizeof(u32)) {
            u32 values[checksums_per_item];
            __builtin_memcpy(values, existing->begin(), sizeof(values));
            for (u64 i = 0; i < here; ++i)
                values[block - group + i] = 0;
            bool any = false;
            for (u64 i = 0; i < checksums_per_item && !any; ++i)
                any = values[i] != 0;
            if (any)
                TRY(m_tree.insert(key, { reinterpret_cast<u8 const*>(values), sizeof(values) }));
            else
                (void)TRY(m_tree.remove(key));
        }
        done += here;
    }
    return {};
}

ErrorOr<Optional<u64>> Volume::find_damage(BlockIndex first, u64 count, u8 const* data)
{
    for (u64 done = 0; done < count;) {
        BlockIndex const block = first + done;
        BlockIndex const group = block - block % checksums_per_item;
        u64 const here = min(count - done, group + checksums_per_item - block);
        auto existing = TRY(m_tree.find(make_key(checksums_object, item_checksums, group)));
        if (!existing.has_value() || existing->size() != checksums_per_item * sizeof(u32))
            return Optional<u64>(done);
        for (u64 i = 0; i < here; ++i) {
            u32 expected;
            __builtin_memcpy(&expected, existing->begin() + (block - group + i) * sizeof(u32), sizeof(u32));
            if (expected != block_checksum(data + (done + i) * block_size))
                return Optional<u64>(done + i);
        }
        done += here;
    }
    return Optional<u64> {};
}

ErrorOr<void> Volume::read_verified(InodeNumber number, u64 file_block, BlockIndex block, u64 count, u8* destination)
{
    TRY(m_store.read_blocks(block, count, { destination, count * block_size }));
    auto damaged = TRY(find_damage(block, count, destination));
    if (damaged.has_value()) {
        if (m_damage_reporter)
            m_damage_reporter(number, file_block + damaged.value(), block + damaged.value());
        return Error::from_errno(EIO);
    }
    return {};
}

ErrorOr<void> Volume::read_file_block(InodeNumber number, u64 file_block, u8* destination)
{
    auto block = TRY(map_block(number, file_block));
    if (!block.has_value()) {
        __builtin_memset(destination, 0, block_size);
        return {};
    }
    return read_verified(number, file_block, block.value(), 1, destination);
}

ErrorOr<void> Volume::read_chunk(InodeNumber number, Extent const& extent)
{
    if (m_chunk_inode == number && m_chunk_block == extent.file_block)
        return {};
    forget_chunk();
    Vector<u8> packed;
    TRY(packed.try_resize(extent.count * block_size));
    TRY(read_verified(number, extent.file_block, extent.block, extent.count, packed.begin()));
    if (extent.compressed_size > packed.size())
        return Error::from_errno(EIO);
    size_t const expected = extent.logical * block_size;
    auto chunk = Compress::inflate({ packed.begin(), extent.compressed_size }, expected);
    if (chunk.is_error() || chunk.value().size() != expected)
        return Error::from_errno(EIO);
    m_chunk = chunk.release_value();
    m_chunk_inode = number;
    m_chunk_block = extent.file_block;
    return {};
}

ErrorOr<size_t> Volume::read(InodeNumber number, u64 offset, Bytes buffer)
{
    auto record = TRY(inode(number));
    if (is_directory(record))
        return Error::from_errno(EISDIR);
    if (is_symbolic_link(record))
        return Error::from_errno(EINVAL);
    return read_any(number, offset, buffer, record);
}

ErrorOr<size_t> Volume::read_any(InodeNumber number, u64 offset, Bytes buffer, InodeRecord const& record)
{
    if (offset >= record.size)
        return 0;
    size_t length = static_cast<size_t>(min<u64>(buffer.size(), record.size - offset));
    Vector<u8> scratch;
    size_t done = 0;
    while (done < length) {
        u64 position = offset + done;
        u64 file_block = position / block_size;
        size_t within = position % block_size;
        size_t chunk = min(block_size - within, length - done);
        auto extent = TRY(find_extent(number, file_block));
        if (!extent.has_value()) {
            __builtin_memset(buffer.data() + done, 0, chunk);
            done += chunk;
            continue;
        }
        if (extent->compression != compression_none) {
            TRY(read_chunk(number, extent.value()));
            u64 const chunk_offset = position - extent->file_block * block_size;
            size_t const here = static_cast<size_t>(min<u64>(extent->logical * block_size - chunk_offset, length - done));
            __builtin_memcpy(buffer.data() + done, m_chunk.begin() + chunk_offset, here);
            done += here;
            continue;
        }
        BlockIndex block = extent->block + (file_block - extent->file_block);
        if (within == 0 && length - done >= block_size) {
            // Whole blocks straight into the caller's buffer.
            u64 blocks = min<u64>(extent->file_block + extent->count - file_block, (length - done) / block_size);
            TRY(read_verified(number, file_block, block, blocks, buffer.data() + done));
            done += blocks * block_size;
            continue;
        }
        if (scratch.is_empty())
            TRY(scratch.try_resize(block_size));
        TRY(read_verified(number, file_block, block, 1, scratch.begin()));
        __builtin_memcpy(buffer.data() + done, scratch.begin() + within, chunk);
        done += chunk;
    }
    return length;
}

ErrorOr<void> Volume::write(InodeNumber number, u64 offset, ReadonlyBytes data)
{
    auto record = TRY(inode(number));
    if (is_directory(record))
        return Error::from_errno(EISDIR);
    if (is_symbolic_link(record))
        return Error::from_errno(EINVAL);
    if (data.is_empty())
        return {};
    if (offset > max_file_size || data.size() > max_file_size - offset)
        return Error::from_errno(EFBIG);
    u64 const total = (offset + data.size() - 1) / block_size - offset / block_size + 1;
    // As well as the data, each of those needs a checksum: a few of those fit in each node of the tree.
    if (m_allocator.available_count() < total + total / 256 + 1 + blocks_to_keep_back() + m_tree.loaded_nodes())
        return Error::from_errno(ENOSPC);
    TRY(mutate());
    if (m_chunk_inode == number)
        forget_chunk();
    if (record.flags & inode_flag_compressed)
        TRY(write_compressed(number, offset, data, record));
    else
        TRY(write_plain(number, offset, data, record));
    record.size = max(record.size, offset + data.size());
    record.modification_time = record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

ErrorOr<void> Volume::write_plain(InodeNumber number, u64 offset, ReadonlyBytes data, InodeRecord& record)
{
    u64 end = offset + data.size();
    u64 first = offset / block_size;
    u64 last = (end - 1) / block_size;
    u64 total = last - first + 1;

    // Write the new contents to free blocks first; nothing refers to them
    // until the extents below do.
    Vector<Extent> fresh;
    Vector<u8> batch;
    auto write_new_blocks = [&]() -> ErrorOr<void> {
        TRY(batch.try_resize(write_batch_blocks * block_size));
        u64 file_block = first;
        BlockIndex near = 0;
        if (first > 0) {
            auto previous = TRY(map_block(number, first - 1));
            if (previous.has_value())
                near = previous.value() + 1;
        }
        while (file_block <= last) {
            u64 wanted = min(last - file_block + 1, write_batch_blocks);
            auto run = near ? TRY(m_allocator.allocate_run(wanted, near)) : TRY(m_allocator.allocate_run(wanted));
            TRY(fresh.try_append(Extent { file_block, run.first, run.count, run.count, compression_none, 0 }));
            for (u64 i = 0; i < run.count; ++i) {
                u64 block_start = (file_block + i) * block_size;
                u8* destination = batch.begin() + i * block_size;
                u64 low = max(offset, block_start);
                u64 high = min(end, block_start + block_size);
                if (low > block_start || high < block_start + block_size)
                    TRY(read_file_block(number, file_block + i, destination));
                __builtin_memcpy(destination + (low - block_start), data.data() + (low - offset), high - low);
            }
            TRY(m_store.write_blocks(run.first, run.count, { batch.begin(), run.count * block_size }));
            TRY(set_checksums(run.first, run.count, batch.begin()));
            file_block += run.count;
            near = run.first + run.count;
        }
        return {};
    };
    if (auto result = write_new_blocks(); result.is_error()) {
        for (auto const& extent : fresh)
            CHANGE(drop_references(extent.block, extent.count));
        return result.release_error();
    }

    u64 freed = CHANGE(punch(number, first, last + 1));
    for (auto const& extent : fresh)
        CHANGE(add_extent(number, extent));
    record.allocated_blocks = record.allocated_blocks - freed + total;
    return {};
}

ErrorOr<void> Volume::put_chunk(InodeNumber number, u64 chunk_block, u8 const* data, size_t size, InodeRecord& record)
{
    u64 const logical = (size + block_size - 1) / block_size;
    Vector<u8> padded;
    TRY(padded.try_resize(logical * block_size));
    __builtin_memset(padded.begin(), 0, padded.size());
    __builtin_memcpy(padded.begin(), data, size);

    // Worth it only if that makes it at least a block smaller, and if it can be kept together.
    auto packed = TRY(Compress::deflate({ padded.begin(), padded.size() }));
    size_t const packed_size = packed.size();
    u64 const packed_blocks = (packed_size + block_size - 1) / block_size;
    Optional<BlockRun> place;
    if (packed_blocks < logical) {
        Vector<BlockRun> too_short;
        for (size_t attempt = 0; attempt < 4 && !place.has_value(); ++attempt) {
            auto run = TRY(m_allocator.allocate_run(packed_blocks));
            if (run.count == packed_blocks)
                place = run;
            else
                TRY(too_short.try_append(run));
        }
        for (auto const& run : too_short)
            m_allocator.free(run.first, run.count);
    }

    if (place.has_value()) {
        TRY(packed.try_resize(packed_blocks * block_size));
        __builtin_memset(packed.begin() + packed_size, 0, packed_blocks * block_size - packed_size);
        auto const written = [&]() -> ErrorOr<void> {
            TRY(m_store.write_blocks(place->first, place->count, { packed.begin(), packed.size() }));
            return set_checksums(place->first, place->count, packed.begin());
        }();
        if (written.is_error()) {
            CHANGE(drop_references(place->first, place->count));
            return written;
        }
        u64 const freed = CHANGE(punch(number, chunk_block, chunk_block + compression_chunk_blocks));
        CHANGE(put_extent(number, Extent { chunk_block, place->first, place->count, logical, compression_deflate, static_cast<u32>(packed_size) }));
        record.allocated_blocks = record.allocated_blocks - freed + place->count;
        return {};
    }

    Vector<Extent> fresh;
    auto const written = [&]() -> ErrorOr<void> {
        for (u64 done = 0; done < logical;) {
            auto run = TRY(m_allocator.allocate_run(logical - done));
            TRY(fresh.try_append(Extent { chunk_block + done, run.first, run.count, run.count, compression_none, 0 }));
            TRY(m_store.write_blocks(run.first, run.count, { padded.begin() + done * block_size, run.count * block_size }));
            TRY(set_checksums(run.first, run.count, padded.begin() + done * block_size));
            done += run.count;
        }
        return {};
    }();
    if (written.is_error()) {
        for (auto const& extent : fresh)
            CHANGE(drop_references(extent.block, extent.count));
        return written;
    }
    u64 const freed = CHANGE(punch(number, chunk_block, chunk_block + compression_chunk_blocks));
    for (auto const& extent : fresh)
        CHANGE(add_extent(number, extent));
    record.allocated_blocks = record.allocated_blocks - freed + logical;
    return {};
}

ErrorOr<void> Volume::write_compressed(InodeNumber number, u64 offset, ReadonlyBytes data, InodeRecord& record)
{
    constexpr u64 chunk_bytes = compression_chunk_blocks * block_size;
    u64 const end = offset + data.size();
    Vector<u8> chunk;
    TRY(chunk.try_resize(chunk_bytes));
    for (u64 start = offset - offset % chunk_bytes; start < end; start += chunk_bytes) {
        u64 const low = max(offset, start);
        u64 const high = min(end, start + chunk_bytes);
        u64 const old_end = min(record.size, start + chunk_bytes);
        size_t const kept = old_end > start ? static_cast<size_t>(old_end - start) : 0;
        __builtin_memset(chunk.begin(), 0, chunk_bytes);
        if (kept && !(low == start && high == start + chunk_bytes))
            TRY(read_any(number, start, { chunk.begin(), kept }, record));
        __builtin_memcpy(chunk.begin() + (low - start), data.data() + (low - offset), high - low);
        size_t const size = max(kept, static_cast<size_t>(high - start));
        TRY(put_chunk(number, start / block_size, chunk.begin(), size, record));
        forget_chunk();
    }
    return {};
}

ErrorOr<void> Volume::truncate(InodeNumber number, u64 size)
{
    auto record = TRY(inode(number));
    if (is_directory(record))
        return Error::from_errno(EISDIR);
    if (is_symbolic_link(record))
        return Error::from_errno(EINVAL);
    if (size > max_file_size)
        return Error::from_errno(EFBIG);
    if (size == record.size)
        return {};
    TRY(mutate());
    if (m_chunk_inode == number)
        forget_chunk();
    if (size < record.size && (record.flags & inode_flag_compressed)) {
        constexpr u64 chunk_bytes = compression_chunk_blocks * block_size;
        u64 const tail = size % chunk_bytes;
        u64 const from = size / chunk_bytes * compression_chunk_blocks;
        Vector<u8> head;
        if (tail) {
            TRY(head.try_resize(tail));
            TRY(read_any(number, size - tail, { head.begin(), tail }, record));
            forget_chunk();
        }
        record.allocated_blocks -= CHANGE(punch(number, from, ~0ull));
        if (tail && TRY(extents_in(number, from, from)).is_empty())
            CHANGE(put_chunk(number, from, head.begin(), tail, record));
    } else if (size < record.size) {
        // Bytes past the end are always zero: clear the rest of the new
        // last block, if it has one on disk.
        u64 tail = size % block_size;
        if (tail != 0 && TRY(map_block(number, size / block_size)).has_value()) {
            u64 clear_end = min(record.size, size - tail + block_size);
            Vector<u8> zeros;
            TRY(zeros.try_resize(clear_end - size));
            __builtin_memset(zeros.begin(), 0, zeros.size());
            TRY(write(number, size, { zeros.begin(), zeros.size() }));
            record = TRY(inode(number));
        }
        u64 freed = CHANGE(punch(number, (size + block_size - 1) / block_size, ~0ull));
        record.allocated_blocks -= freed;
    }
    record.size = size;
    record.modification_time = record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

ErrorOr<void> Volume::set_compressed(InodeNumber number, bool compressed)
{
    auto record = TRY(inode(number));
    if (!is_regular(record) || (record.flags & inode_flag_stream))
        return Error::from_errno(EINVAL);
    if (static_cast<bool>(record.flags & inode_flag_compressed) == compressed)
        return {};
    u64 const blocks = (record.size + block_size - 1) / block_size;
    if (m_allocator.available_count() < blocks + blocks / 256 + 1 + blocks_to_keep_back() + m_tree.loaded_nodes())
        return Error::from_errno(ENOSPC);
    TRY(mutate());
    constexpr u64 chunk_bytes = compression_chunk_blocks * block_size;
    Vector<u8> chunk;
    TRY(chunk.try_resize(chunk_bytes));
    for (u64 start = 0; start < record.size; start += chunk_bytes) {
        u64 const first = start / block_size;
        // A hole stays one.
        if (TRY(extents_in(number, first, first + compression_chunk_blocks)).is_empty())
            continue;
        size_t const kept = static_cast<size_t>(min(chunk_bytes, record.size - start));
        TRY(read_any(number, start, { chunk.begin(), kept }, record));
        forget_chunk();
        if (compressed)
            CHANGE(put_chunk(number, first, chunk.begin(), kept, record));
        else
            CHANGE(write_plain(number, start, { chunk.begin(), kept }, record));
    }
    if (compressed)
        record.flags |= inode_flag_compressed;
    else
        record.flags &= ~inode_flag_compressed;
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

ErrorOr<size_t> Volume::read_link(InodeNumber number, Bytes buffer)
{
    auto record = TRY(inode(number));
    if (!is_symbolic_link(record))
        return Error::from_errno(EINVAL);
    auto target = TRY(m_tree.find(make_key(number, item_inline_data, 0)));
    if (!target.has_value())
        return Error::from_string_literal("vultureFS: symbolic link without a target");
    size_t length = min(buffer.size(), target->size());
    __builtin_memcpy(buffer.data(), target->begin(), length);
    return length;
}

ErrorOr<void> Volume::set_permissions(InodeNumber number, u32 permissions)
{
    auto record = TRY(inode(number));
    TRY(mutate());
    record.mode = (record.mode & mode_type_mask) | (permissions & 07777);
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

ErrorOr<void> Volume::set_owner(InodeNumber number, u32 uid, u32 gid)
{
    auto record = TRY(inode(number));
    TRY(mutate());
    record.uid = uid;
    record.gid = gid;
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

ErrorOr<void> Volume::set_times(InodeNumber number, i64 access_time, i64 modification_time)
{
    auto record = TRY(inode(number));
    TRY(mutate());
    record.access_time = access_time;
    record.modification_time = modification_time;
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

ErrorOr<void> Volume::set_bsd_flags(InodeNumber number, u32 flags)
{
    auto record = TRY(inode(number));
    if (record.reserved == flags)
        return {};
    TRY(mutate());
    record.reserved = flags;
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

// ---- Clones.

ErrorOr<void> Volume::copy_contents(InodeNumber source, InodeNumber target, InodeRecord const& record)
{
    for (auto const& extent : TRY(extents_in(source, 0, ~0ull))) {
        TRY(add_references(extent.block, extent.count));
        TRY(put_extent(target, extent));
    }
    if (is_symbolic_link(record)) {
        auto link_target = TRY(m_tree.find(make_key(source, item_inline_data, 0)));
        if (link_target.has_value())
            TRY(m_tree.insert(make_key(target, item_inline_data, 0), { link_target->begin(), link_target->size() }));
    }
    auto acl_value = TRY(m_tree.find(make_key(source, item_acl, 0)));
    if (acl_value.has_value())
        TRY(m_tree.insert(make_key(target, item_acl, 0), { acl_value->begin(), acl_value->size() }));

    Vector<Item> xattrs;
    TRY(m_tree.for_each_from(make_key(source, item_xattr, 0), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != source || key.type != item_xattr)
            return Iteration::Stop;
        Item item { key, {} };
        MUST(item.value.try_resize(value.size()));
        __builtin_memcpy(item.value.begin(), value.data(), value.size());
        xattrs.append(move(item));
        return Iteration::Continue;
    }));
    for (auto& item : xattrs) {
        // A value in a file of its own gets a clone of that.
        for (size_t offset = 0; offset + sizeof(XattrRecord) <= item.value.size();) {
            XattrRecord xattr;
            __builtin_memcpy(&xattr, item.value.begin() + offset, sizeof(xattr));
            if (xattr.flags & xattr_in_stream) {
                xattr.stream_inode = TRY(clone_inode(xattr.stream_inode, 0, 0, 0));
                __builtin_memcpy(item.value.begin() + offset, &xattr, sizeof(xattr));
            }
            offset += sizeof(xattr) + xattr.name_length + xattr.inline_size;
        }
        TRY(m_tree.insert(make_key(target, item_xattr, item.key.offset), { item.value.begin(), item.value.size() }));
    }
    return {};
}

ErrorOr<InodeNumber> Volume::clone_inode(InodeNumber source, u32 uid, u32 gid, InodeNumber parent)
{
    auto record = TRY(inode(source));
    InodeNumber const number = m_superblock.next_object++;
    InodeRecord copy = record;
    copy.uid = uid;
    copy.gid = gid;
    copy.change_time = now();
    if (is_directory(record)) {
        copy.link_count = 2;
        copy.size = 0;
        copy.parent = parent;
    } else {
        copy.link_count = 1;
        copy.parent = 0;
        // Something that says who it runs as isn't to say so about someone else's copy of it.
        copy.mode &= ~06000u;
    }
    TRY(put_inode(number, copy));
    TRY(copy_contents(source, number, record));
    return number;
}

ErrorOr<InodeNumber> Volume::clone(InodeNumber source, InodeNumber directory_inode, StringView name, u32 uid, u32 gid)
{
    TRY(validate_name(name));
    auto parent = TRY(directory(directory_inode));
    if (TRY(find_entry(directory_inode, name)).has_value())
        return Error::from_errno(EEXIST);
    auto record = TRY(inode(source));
    if (is_directory(record) && parent.link_count >= max_link_count)
        return Error::from_errno(EMLINK);
    if (m_allocator.available_count() < blocks_to_keep_back() + m_tree.loaded_nodes())
        return Error::from_errno(ENOSPC);
    TRY(mutate());

    InodeNumber const number = CHANGE(clone_inode(source, uid, gid, directory_inode));
    CHANGE(add_entry(directory_inode, name, { number, directory_type_for_mode(record.mode) }));
    CHANGE(adjust_directory(directory_inode, 1, is_directory(record) ? 1 : 0));

    // A directory, all the way down: one at a time, as a tree can be deeper than a kernel's stack would like.
    struct Pair {
        InodeNumber source;
        InodeNumber target;
    };
    struct Child {
        String name;
        DirectoryEntry entry;
    };
    Vector<Pair> waiting;
    if (is_directory(record))
        TRY(waiting.try_append(Pair { source, number }));
    while (!waiting.is_empty()) {
        Pair const pair = waiting.take_last();
        Vector<Child> children;
        TRY(for_each_entry(pair.source, [&](StringView child_name, DirectoryEntry entry) {
            children.append(Child { MUST(String::from_view(child_name)), entry });
            return Iteration::Continue;
        }));
        for (auto const& child : children) {
            bool const is_subdirectory = child.entry.type == type_directory;
            InodeNumber const copy = CHANGE(clone_inode(child.entry.inode, uid, gid, pair.target));
            CHANGE(add_entry(pair.target, child.name.view(), { copy, child.entry.type }));
            CHANGE(adjust_directory(pair.target, 1, is_subdirectory ? 1 : 0));
            if (is_subdirectory)
                TRY(waiting.try_append(Pair { child.entry.inode, copy }));
        }
        // Having things put in it is no change to it, as far as anyone looking at the original can tell.
        auto target = TRY(inode(pair.target));
        auto const original = TRY(inode(pair.source));
        target.modification_time = original.modification_time;
        CHANGE(put_inode(pair.target, target));
    }
    return number;
}

// ---- Extended attributes.

static u64 xattr_hash(StringView name)
{
    return name_hash(name.characters_without_null_termination(), name.length());
}

ErrorOr<size_t> Volume::get_xattr(InodeNumber number, StringView name, Bytes buffer)
{
    (void)TRY(inode(number));
    auto value = TRY(m_tree.find(make_key(number, item_xattr, xattr_hash(name))));
    if (!value.has_value())
        return Error::from_errno(ENOATTR);
    auto const& bytes = value.value();
    for (size_t offset = 0; offset + sizeof(XattrRecord) <= bytes.size();) {
        XattrRecord record;
        __builtin_memcpy(&record, bytes.begin() + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.name_length + record.inline_size > bytes.size())
            break;
        StringView stored { reinterpret_cast<char const*>(bytes.begin() + offset), record.name_length };
        offset += record.name_length;
        if (stored != name) {
            offset += record.inline_size;
            continue;
        }
        u64 const size = (record.flags & xattr_in_stream) ? record.stream_size : record.inline_size;
        if (buffer.is_empty())
            return static_cast<size_t>(size);
        if (buffer.size() < size)
            return Error::from_errno(ERANGE);
        if (record.flags & xattr_in_stream) {
            auto const stream = TRY(inode(record.stream_inode));
            return read_any(record.stream_inode, 0, buffer.trim(static_cast<size_t>(size)), stream);
        }
        __builtin_memcpy(buffer.data(), bytes.begin() + offset, record.inline_size);
        return static_cast<size_t>(size);
    }
    return Error::from_errno(ENOATTR);
}

ErrorOr<void> Volume::set_xattr(InodeNumber number, StringView name, ReadonlyBytes value, XattrMode mode)
{
    if (name.is_empty())
        return Error::from_errno(EINVAL);
    if (name.length() > max_xattr_name_length)
        return Error::from_errno(ENAMETOOLONG);
    auto record = TRY(inode(number));
    auto const key = make_key(number, item_xattr, xattr_hash(name));
    auto existing = TRY(m_tree.find(key));

    // Whatever else happens to be under this hash stays.
    Vector<u8> kept;
    Optional<InodeNumber> old_stream;
    bool found = false;
    if (existing.has_value()) {
        auto const& bytes = existing.value();
        for (size_t offset = 0; offset + sizeof(XattrRecord) <= bytes.size();) {
            XattrRecord old;
            __builtin_memcpy(&old, bytes.begin() + offset, sizeof(old));
            size_t const length = sizeof(old) + old.name_length + old.inline_size;
            if (offset + length > bytes.size())
                break;
            StringView stored { reinterpret_cast<char const*>(bytes.begin() + offset + sizeof(old)), old.name_length };
            if (stored == name) {
                found = true;
                if (old.flags & xattr_in_stream)
                    old_stream = old.stream_inode;
            } else {
                for (size_t i = 0; i < length; ++i)
                    TRY(kept.try_append(bytes[offset + i]));
            }
            offset += length;
        }
    }
    if (found && mode == XattrMode::MustBeNew)
        return Error::from_errno(EEXIST);
    if (!found && mode == XattrMode::MustExist)
        return Error::from_errno(ENOATTR);

    bool const inline_value = value.size() <= max_inline_xattr_size && kept.size() + sizeof(XattrRecord) + name.length() + value.size() <= max_value_size;
    if (!inline_value) {
        u64 const blocks = value.size() / block_size + 1;
        if (m_allocator.available_count() < blocks + blocks / 256 + 1 + blocks_to_keep_back() + m_tree.loaded_nodes())
            return Error::from_errno(ENOSPC);
    }
    TRY(mutate());

    XattrRecord fresh {};
    fresh.name_length = static_cast<u16>(name.length());
    if (inline_value) {
        fresh.inline_size = static_cast<u32>(value.size());
    } else {
        InodeNumber const stream = m_superblock.next_object++;
        InodeRecord stream_record {};
        stream_record.mode = mode_regular;
        stream_record.link_count = 1;
        stream_record.flags = inode_flag_stream;
        stream_record.access_time = stream_record.modification_time = stream_record.change_time = stream_record.birth_time = now();
        CHANGE(put_inode(stream, stream_record));
        if (!value.is_empty())
            CHANGE(write(stream, 0, value));
        fresh.flags = xattr_in_stream;
        fresh.stream_inode = stream;
        fresh.stream_size = value.size();
    }
    size_t const at = kept.size();
    size_t const added = sizeof(fresh) + name.length() + (inline_value ? value.size() : 0);
    TRY(kept.try_resize(at + added));
    __builtin_memcpy(kept.begin() + at, &fresh, sizeof(fresh));
    __builtin_memcpy(kept.begin() + at + sizeof(fresh), name.characters_without_null_termination(), name.length());
    if (inline_value && !value.is_empty())
        __builtin_memcpy(kept.begin() + at + sizeof(fresh) + name.length(), value.data(), value.size());
    CHANGE(m_tree.insert(key, { kept.begin(), kept.size() }));
    if (old_stream.has_value()) {
        (void)CHANGE(punch(old_stream.value(), 0, ~0ull));
        (void)CHANGE(m_tree.remove(make_key(old_stream.value(), item_inode, 0)));
    }
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

ErrorOr<void> Volume::remove_xattr(InodeNumber number, StringView name)
{
    auto record = TRY(inode(number));
    auto const key = make_key(number, item_xattr, xattr_hash(name));
    auto existing = TRY(m_tree.find(key));
    if (!existing.has_value())
        return Error::from_errno(ENOATTR);
    auto const& bytes = existing.value();
    Vector<u8> kept;
    Optional<InodeNumber> old_stream;
    bool found = false;
    for (size_t offset = 0; offset + sizeof(XattrRecord) <= bytes.size();) {
        XattrRecord old;
        __builtin_memcpy(&old, bytes.begin() + offset, sizeof(old));
        size_t const length = sizeof(old) + old.name_length + old.inline_size;
        if (offset + length > bytes.size())
            break;
        StringView stored { reinterpret_cast<char const*>(bytes.begin() + offset + sizeof(old)), old.name_length };
        if (stored == name) {
            found = true;
            if (old.flags & xattr_in_stream)
                old_stream = old.stream_inode;
        } else {
            for (size_t i = 0; i < length; ++i)
                TRY(kept.try_append(bytes[offset + i]));
        }
        offset += length;
    }
    if (!found)
        return Error::from_errno(ENOATTR);
    TRY(mutate());
    if (kept.is_empty())
        (void)CHANGE(m_tree.remove(key));
    else
        CHANGE(m_tree.insert(key, { kept.begin(), kept.size() }));
    if (old_stream.has_value()) {
        (void)CHANGE(punch(old_stream.value(), 0, ~0ull));
        (void)CHANGE(m_tree.remove(make_key(old_stream.value(), item_inode, 0)));
    }
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

// ---- Access control lists.

ErrorOr<Vector<AclEntry>> Volume::acl(InodeNumber number)
{
    Vector<AclEntry> entries;
    auto value = TRY(m_tree.find(make_key(number, item_acl, 0)));
    if (!value.has_value())
        return entries;
    size_t const count = value->size() / sizeof(AclEntry);
    TRY(entries.try_resize(count));
    __builtin_memcpy(entries.begin(), value->begin(), count * sizeof(AclEntry));
    return entries;
}

ErrorOr<void> Volume::set_acl(InodeNumber number, Span<AclEntry const> entries)
{
    if (entries.size() > max_acl_entries)
        return Error::from_errno(EINVAL);
    auto record = TRY(inode(number));
    TRY(mutate());
    if (entries.is_empty()) {
        (void)CHANGE(m_tree.remove(make_key(number, item_acl, 0)));
        record.flags &= ~inode_flag_has_acl;
    } else {
        CHANGE(m_tree.insert(make_key(number, item_acl, 0), { reinterpret_cast<u8 const*>(entries.data()), entries.size() * sizeof(AclEntry) }));
        record.flags |= inode_flag_has_acl;
    }
    record.change_time = now();
    CHANGE(put_inode(number, record));
    return {};
}

// ---- Snapshots.

ErrorOr<Optional<u64>> Volume::find_snapshot(StringView name)
{
    Optional<u64> found;
    TRY(m_tree.for_each_from(make_key(snapshots_object, item_snapshot, 0), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != snapshots_object || key.type != item_snapshot)
            return Iteration::Stop;
        if (value.size() < sizeof(SnapshotRecord))
            return Iteration::Continue;
        StringView stored { reinterpret_cast<char const*>(value.data() + sizeof(SnapshotRecord)), value.size() - sizeof(SnapshotRecord) };
        if (stored != name)
            return Iteration::Continue;
        found = key.offset;
        return Iteration::Stop;
    }));
    return found;
}

ErrorOr<Vector<SnapshotInfo>> Volume::snapshots()
{
    Vector<SnapshotInfo> list;
    TRY(m_tree.for_each_from(make_key(snapshots_object, item_snapshot, 0), [&](Key const& key, ReadonlyBytes value) {
        if (key.object != snapshots_object || key.type != item_snapshot)
            return Iteration::Stop;
        if (value.size() < sizeof(SnapshotRecord))
            return Iteration::Continue;
        SnapshotRecord record;
        __builtin_memcpy(&record, value.data(), sizeof(record));
        StringView stored { reinterpret_cast<char const*>(value.data() + sizeof(record)), value.size() - sizeof(record) };
        list.append(SnapshotInfo { key.offset, MUST(String::from_view(stored)), record.time, record.generation });
        return Iteration::Continue;
    }));
    return list;
}

ErrorOr<void> Volume::create_snapshot(StringView name)
{
    if (name.is_empty() || name.contains('/'))
        return Error::from_errno(EINVAL);
    if (name.length() > max_snapshot_name_length)
        return Error::from_errno(ENAMETOOLONG);
    if (TRY(find_snapshot(name)).has_value())
        return Error::from_errno(EEXIST);
    if (m_read_only)
        return Error::from_errno(EROFS);
    if (m_allocator.available_count() < 1 + blocks_to_keep_back() + m_tree.loaded_nodes())
        return Error::from_errno(ENOSPC);

    // This moment needs to be one the disk has. Then what it's made of is kept, by a note of it in the next.
    TRY(checkpoint());
    Superblock const saved = m_superblock;
    TRY(mutate());
    BlockIndex const block = CHANGE(m_allocator.allocate());
    Vector<u8> bytes;
    TRY(bytes.try_resize(block_size));
    __builtin_memset(bytes.begin(), 0, block_size);
    __builtin_memcpy(bytes.begin(), &saved, sizeof(saved));
    CHANGE(m_store.write_blocks(block, 1, { bytes.begin(), block_size }));

    Vector<u8> value;
    TRY(value.try_resize(sizeof(SnapshotRecord) + name.length()));
    SnapshotRecord record {};
    record.saved_superblock = block;
    record.saved_checksum = crc32c({ bytes.begin(), block_size });
    record.time = now();
    record.generation = saved.generation;
    __builtin_memcpy(value.begin(), &record, sizeof(record));
    __builtin_memcpy(value.begin() + sizeof(record), name.characters_without_null_termination(), name.length());
    CHANGE(m_tree.insert(make_key(snapshots_object, item_snapshot, m_superblock.next_snapshot++), { value.begin(), value.size() }));
    CHANGE(m_allocator.add_snapshot(m_store, saved));
    return checkpoint();
}

ErrorOr<void> Volume::delete_snapshot(StringView name)
{
    auto id = TRY(find_snapshot(name));
    if (!id.has_value())
        return Error::from_errno(ENOENT);
    auto value = TRY(m_tree.find(make_key(snapshots_object, item_snapshot, id.value())));
    SnapshotRecord record;
    __builtin_memcpy(&record, value->begin(), sizeof(record));
    TRY(mutate());
    (void)CHANGE(m_tree.remove(make_key(snapshots_object, item_snapshot, id.value())));
    m_allocator.free(record.saved_superblock, 1);
    if (m_superblock.pending_rollback == id.value())
        m_superblock.pending_rollback = 0;
    // What only it was keeping can go, once the last checkpoint that says it's there has been replaced.
    TRY(checkpoint());
    return load_snapshots();
}

ErrorOr<void> Volume::request_rollback(StringView name)
{
    auto id = TRY(find_snapshot(name));
    if (!id.has_value())
        return Error::from_errno(ENOENT);
    TRY(mutate());
    m_superblock.pending_rollback = id.value();
    return checkpoint();
}

ErrorOr<void> Volume::go_back(u64 snapshot)
{
    auto const saved = TRY(saved_superblock(snapshot));
    auto const old_value = TRY(m_tree.find(make_key(snapshots_object, item_snapshot, snapshot)));
    m_dirty = true;
    if (!saved.has_value() || !old_value.has_value()) {
        m_superblock.pending_rollback = 0;
        return checkpoint();
    }

    // Nothing here lasts until the checkpoint at the end does: a crash before then will find itself asked to do this again.
    TRY(m_allocator.go_back_to(m_store, saved.value()));
    TRY(m_tree.load(saved->tree_root, saved->tree_root_generation));
    m_superblock.next_object = saved->next_object;
    m_superblock.oldest_event = saved->oldest_event;
    m_superblock.pending_rollback = 0;
    forget_chunk();

    // That checkpoint didn't know about the snapshot of itself: this one does, with a copy of its own.
    BlockIndex const block = TRY(m_allocator.allocate());
    Vector<u8> bytes;
    TRY(bytes.try_resize(block_size));
    __builtin_memset(bytes.begin(), 0, block_size);
    __builtin_memcpy(bytes.begin(), &saved.value(), sizeof(Superblock));
    TRY(m_store.write_blocks(block, 1, { bytes.begin(), block_size }));
    Vector<u8> value;
    TRY(value.try_resize(old_value->size()));
    __builtin_memcpy(value.begin(), old_value->begin(), old_value->size());
    SnapshotRecord record;
    __builtin_memcpy(&record, value.begin(), sizeof(record));
    record.saved_superblock = block;
    record.saved_checksum = crc32c({ bytes.begin(), block_size });
    __builtin_memcpy(value.begin(), &record, sizeof(record));
    TRY(m_tree.insert(make_key(snapshots_object, item_snapshot, snapshot), { value.begin(), value.size() }));
    TRY(checkpoint());
    return load_snapshots();
}

// ---- What's changed.

ErrorOr<u64> Volume::append_event(u32 flags, StringView path)
{
    TRY(mutate());
    u64 const id = m_superblock.next_event++;
    Vector<u8> value;
    TRY(value.try_resize(sizeof(EventRecord) + path.length()));
    EventRecord record {};
    record.flags = flags;
    __builtin_memcpy(value.begin(), &record, sizeof(record));
    __builtin_memcpy(value.begin() + sizeof(record), path.characters_without_null_termination(), path.length());
    CHANGE(m_tree.insert(make_key(events_object, item_event, id), { value.begin(), value.size() }));
    while (m_superblock.next_event - m_superblock.oldest_event > max_kept_events)
        (void)CHANGE(m_tree.remove(make_key(events_object, item_event, m_superblock.oldest_event++)));
    return id;
}

ErrorOr<void> Volume::checkpoint()
{
    if (m_read_only)
        return {};
    if (m_failed)
        return Error::from_errno(EIO);
    if (!m_dirty)
        return {};
    // 1. The tree, bottom up, and the bitmap, all to free blocks.
    // 2. Flush, so all of it is on disk before ...
    // 3. ... the superblock that points at it, in the other slot.
    // 4. Flush again: now it is committed.
    u64 generation = m_superblock.generation + 1;
    Superblock next = m_superblock;
    auto commit = [&]() -> ErrorOr<void> {
        TRY(m_tree.write(generation));
        next.generation = generation;
        next.tree_root = m_tree.root_block();
        next.tree_root_generation = m_tree.root_generation();
        next.checkpoint_time = now();
        TRY(m_allocator.write(m_store, next));
        TRY(m_store.flush());

        Vector<u8> block;
        TRY(block.try_resize(block_size));
        __builtin_memset(block.begin(), 0, block_size);
        next.checksum = 0;
        __builtin_memcpy(block.begin(), &next, sizeof(next));
        next.checksum = crc32c({ block.begin(), block_size });
        __builtin_memcpy(block.begin(), &next, sizeof(next));
        TRY(m_store.write_blocks(generation % 2, 1, { block.begin(), block_size }));
        TRY(m_store.flush());
        return {};
    };
    if (auto result = commit(); result.is_error()) {
        m_failed = true;
        return result.release_error();
    }
    m_allocator.commit();
    m_superblock = next;
    m_dirty = false;
    m_tree.trim_cache(keep_tree_nodes);
    return {};
}

}
