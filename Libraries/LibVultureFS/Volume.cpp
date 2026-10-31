/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/Checksum.h>
#include <LibVultureFS/Volume.h>

namespace VultureFS {

// File data is written in batches of up to this many blocks.
static constexpr u64 write_batch_blocks = 32;
// Blocks kept back from file data so a checkpoint always has room to
// write the tree and the bitmap.
static constexpr u64 reserved_blocks = 64;
static constexpr u64 max_file_size = 1ull << 44;
static constexpr u32 max_link_count = 65000;
static constexpr size_t keep_tree_nodes = 4096;

static bool is_directory(InodeRecord const& record) { return (record.mode & mode_type_mask) == mode_directory; }
static bool is_symbolic_link(InodeRecord const& record) { return (record.mode & mode_type_mask) == mode_symbolic_link; }

static ErrorOr<void> validate_name(StringView name)
{
    if (name.is_empty() || name == "."_sv || name == ".."_sv)
        return Error::from_errno(EINVAL);
    if (name.length() > max_name_length)
        return Error::from_errno(ENAMETOOLONG);
    for (char c : name) {
        if (c == '/' || c == '\0')
            return Error::from_errno(EINVAL);
    }
    return {};
}

static Key directory_entry_key(InodeNumber directory, StringView name)
{
    return make_key(directory, item_directory_entry, name_hash(name.characters_without_null_termination(), name.length()));
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

ErrorOr<NonnullOwnPtr<Volume>> Volume::format(BlockStore& store, StringView name, ReadonlyBytes uuid, Clock clock)
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
    superblock.created_time = volume->now();
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

static bool read_superblock(u8 const* block, u64 store_blocks, Superblock& superblock)
{
    __builtin_memcpy(&superblock, block, sizeof(superblock));
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
        && superblock.block_count <= max_volume_blocks && superblock.bitmap_block_count <= max_bitmap_blocks;
}

ErrorOr<NonnullOwnPtr<Volume>> Volume::mount(BlockStore& store, Clock clock)
{
    Vector<u8> buffer;
    TRY(buffer.try_resize(2 * block_size));
    TRY(store.read_blocks(0, 2, { buffer.begin(), buffer.size() }));
    Superblock candidates[2];
    bool valid[2];
    for (size_t i = 0; i < 2; ++i)
        valid[i] = read_superblock(buffer.begin() + i * block_size, store.block_count(), candidates[i]);
    if (!valid[0] && !valid[1])
        return Error::from_string_literal("vultureFS: no valid superblock");

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
        auto* raw = new (Base::nothrow) Volume(store, clock);
        if (!raw)
            return Error::from_errno(ENOMEM);
        auto volume = adopt_own(*raw);
        auto result = volume->mount_checkpoint(candidates[slot]);
        if (result.is_error()) {
            last_error = result.release_error();
            continue;
        }
        TRY(volume->delete_orphans());
        return volume;
    }
    return last_error;
}

ErrorOr<void> Volume::mount_checkpoint(Superblock const& superblock)
{
    m_superblock = superblock;
    TRY(m_allocator.load(m_store, superblock));
    TRY(m_tree.load(superblock.tree_root, superblock.tree_root_generation));
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
    return { m_superblock.generation, m_allocator.block_count(), m_allocator.free_count(), m_tree.height() };
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
    if (m_failed)
        return Error::from_errno(EIO);
    m_dirty = true;
    return {};
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

ErrorOr<Optional<DirectoryEntry>> Volume::find_entry(InodeNumber directory, StringView name)
{
    auto value = TRY(m_tree.find(directory_entry_key(directory, name)));
    if (!value.has_value())
        return Optional<DirectoryEntry> {};
    auto const& bytes = value.value();
    for (size_t offset = 0; offset + sizeof(DirectoryEntryRecord) <= bytes.size();) {
        DirectoryEntryRecord record;
        __builtin_memcpy(&record, bytes.begin() + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.name_length > bytes.size())
            break;
        if (StringView(reinterpret_cast<char const*>(bytes.begin() + offset), record.name_length) == name)
            return Optional<DirectoryEntry>(DirectoryEntry { record.inode, record.type });
        offset += record.name_length;
    }
    return Optional<DirectoryEntry> {};
}

ErrorOr<void> Volume::add_entry(InodeNumber directory, StringView name, DirectoryEntry entry)
{
    auto key = directory_entry_key(directory, name);
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

ErrorOr<void> Volume::remove_entry(InodeNumber directory, StringView name)
{
    auto key = directory_entry_key(directory, name);
    auto existing = TRY(m_tree.find(key));
    if (!existing.has_value())
        return Error::from_errno(ENOENT);
    auto const& bytes = existing.value();
    Vector<u8> kept;
    bool found = false;
    for (size_t offset = 0; offset + sizeof(DirectoryEntryRecord) <= bytes.size();) {
        DirectoryEntryRecord record;
        __builtin_memcpy(&record, bytes.begin() + offset, sizeof(record));
        size_t length = sizeof(record) + record.name_length;
        if (offset + length > bytes.size())
            break;
        if (!found && StringView(reinterpret_cast<char const*>(bytes.begin() + offset + sizeof(record)), record.name_length) == name) {
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
    if (name.length() > max_name_length)
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
    if (type != mode_regular && type != mode_directory && type != mode_symbolic_link)
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

    auto existing = TRY(find_entry(new_directory, new_name));
    Optional<InodeRecord> replaced_record;
    if (existing.has_value()) {
        if (existing->inode == entry->inode)
            return Optional<Unlinked> {};
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
    CHANGE(add_entry(new_directory, new_name, entry.value()));
    i64 links = moving_directory && old_directory != new_directory ? 1 : 0;
    CHANGE(adjust_directory(old_directory, -1, -links));
    CHANGE(adjust_directory(new_directory, 1, links));
    if (moving_directory)
        record.parent = new_directory;
    record.change_time = now();
    CHANGE(put_inode(entry->inode, record));
    return result;
}

ErrorOr<void> Volume::release(InodeNumber number)
{
    auto record = TRY(inode(number));
    if (record.link_count != 0)
        return {};
    TRY(mutate());
    (void)CHANGE(punch(number, 0, ~0ull));
    (void)CHANGE(m_tree.remove(make_key(number, item_inline_data, 0)));
    (void)CHANGE(m_tree.remove(make_key(number, item_inode, 0)));
    (void)CHANGE(m_tree.remove(make_key(orphan_list_object, item_orphan, number)));
    return {};
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
    if (file_block - item->key.offset >= record.count)
        return Optional<Extent> {};
    return Optional<Extent>(Extent { item->key.offset, record.block, record.count });
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
        extents.append(Extent { key.offset, record.block, record.count });
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
        u64 extent_end = extent.file_block + extent.count;
        TRY(m_tree.remove(make_key(number, item_extent, extent.file_block)));
        u64 low = max(extent.file_block, first);
        u64 high = min(extent_end, end);
        m_allocator.free(extent.block + (low - extent.file_block), high - low);
        freed += high - low;
        if (extent.file_block < low) {
            ExtentRecord head { extent.block, low - extent.file_block };
            TRY(m_tree.insert(make_key(number, item_extent, extent.file_block), { reinterpret_cast<u8 const*>(&head), sizeof(head) }));
        }
        if (high < extent_end) {
            ExtentRecord tail { extent.block + (high - extent.file_block), extent_end - high };
            TRY(m_tree.insert(make_key(number, item_extent, high), { reinterpret_cast<u8 const*>(&tail), sizeof(tail) }));
        }
    }
    return freed;
}

ErrorOr<void> Volume::add_extent(InodeNumber number, Extent extent)
{
    // Grow the extent just before when this continues it on disk too.
    if (extent.file_block > 0) {
        auto previous = TRY(find_extent(number, extent.file_block - 1));
        if (previous.has_value() && previous->block + previous->count == extent.block) {
            ExtentRecord merged { previous->block, previous->count + extent.count };
            return m_tree.insert(make_key(number, item_extent, previous->file_block), { reinterpret_cast<u8 const*>(&merged), sizeof(merged) });
        }
    }
    ExtentRecord record { extent.block, extent.count };
    return m_tree.insert(make_key(number, item_extent, extent.file_block), { reinterpret_cast<u8 const*>(&record), sizeof(record) });
}

ErrorOr<Optional<BlockIndex>> Volume::map_block(InodeNumber number, u64 file_block)
{
    auto extent = TRY(find_extent(number, file_block));
    if (!extent.has_value())
        return Optional<BlockIndex> {};
    return Optional<BlockIndex>(extent->block + (file_block - extent->file_block));
}

ErrorOr<void> Volume::read_file_block(InodeNumber number, u64 file_block, u8* destination)
{
    auto block = TRY(map_block(number, file_block));
    if (!block.has_value()) {
        __builtin_memset(destination, 0, block_size);
        return {};
    }
    return m_store.read_blocks(block.value(), 1, { destination, block_size });
}

ErrorOr<size_t> Volume::read(InodeNumber number, u64 offset, Bytes buffer)
{
    auto record = TRY(inode(number));
    if (is_directory(record))
        return Error::from_errno(EISDIR);
    if (is_symbolic_link(record))
        return Error::from_errno(EINVAL);
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
        BlockIndex block = extent->block + (file_block - extent->file_block);
        if (within == 0 && length - done >= block_size) {
            // Whole blocks straight into the caller's buffer.
            u64 blocks = min<u64>(extent->file_block + extent->count - file_block, (length - done) / block_size);
            TRY(m_store.read_blocks(block, blocks, buffer.slice(done, blocks * block_size)));
            done += blocks * block_size;
            continue;
        }
        if (scratch.is_empty())
            TRY(scratch.try_resize(block_size));
        TRY(m_store.read_blocks(block, 1, { scratch.begin(), block_size }));
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
    u64 end = offset + data.size();
    u64 first = offset / block_size;
    u64 last = (end - 1) / block_size;
    u64 total = last - first + 1;
    if (m_allocator.available_count() < total + reserved_blocks + m_tree.loaded_nodes())
        return Error::from_errno(ENOSPC);
    TRY(mutate());

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
            TRY(fresh.try_append(Extent { file_block, run.first, run.count }));
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
            file_block += run.count;
            near = run.first + run.count;
        }
        return {};
    };
    if (auto result = write_new_blocks(); result.is_error()) {
        for (auto const& extent : fresh)
            m_allocator.free(extent.block, extent.count);
        return result.release_error();
    }

    u64 freed = CHANGE(punch(number, first, last + 1));
    for (auto const& extent : fresh)
        CHANGE(add_extent(number, extent));
    record.allocated_blocks = record.allocated_blocks - freed + total;
    record.size = max(record.size, end);
    record.modification_time = record.change_time = now();
    CHANGE(put_inode(number, record));
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
    if (size < record.size) {
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

ErrorOr<void> Volume::checkpoint()
{
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
