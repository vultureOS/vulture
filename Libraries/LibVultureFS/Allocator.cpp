/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Checksum.h>
#include <LibVultureFS/Allocator.h>

namespace VultureFS {

static constexpr size_t words_per_bitmap_block = block_size / sizeof(u64);
static constexpr int enospc = 28;

ErrorOr<void> Allocator::initialize(u64 block_count)
{
    if (block_count < 16 || block_count > max_volume_blocks)
        return Error::from_string_literal("vultureFS: unsupported volume size");
    m_block_count = block_count;
    m_bitmap_blocks = (block_count + blocks_per_bitmap_block - 1) / blocks_per_bitmap_block;
    m_index_blocks = (m_bitmap_blocks + bitmap_pointers_per_index_block - 1) / bitmap_pointers_per_index_block;
    size_t words = m_bitmap_blocks * words_per_bitmap_block;
    TRY(m_used.try_resize(words));
    TRY(m_committed.try_resize(words));
    TRY(m_locations.try_resize(m_bitmap_blocks));
    TRY(m_index_locations.try_resize(m_index_blocks));
    TRY(m_dirty.try_resize(m_bitmap_blocks));
    m_held.clear();
    for (size_t i = 0; i < words; ++i)
        m_used[i] = m_committed[i] = 0;
    // Bits past the end of the volume read as used, so nothing allocates
    // them; they are not counted.
    for (u64 block = block_count; block < words * 64; ++block) {
        m_used[block / 64] |= 1ull << (block % 64);
        m_committed[block / 64] |= 1ull << (block % 64);
    }
    m_used_count = 0;
    m_held_count = 0;
    m_pending_frees = 0;
    for (size_t i = 0; i < m_bitmap_blocks; ++i) {
        m_locations[i] = {};
        m_dirty[i] = 1;
    }
    for (size_t i = 0; i < m_index_blocks; ++i)
        m_index_locations[i] = {};
    set_used(0, true);
    set_used(1, true);
    m_committed[0] |= 3;
    return {};
}

ErrorOr<void> Allocator::read_bitmap(BlockStore& store, Superblock const& superblock, Vector<u64>& words) const
{
    if (superblock.bitmap_block_count != m_bitmap_blocks)
        return Error::from_string_literal("vultureFS: bitmap size does not match the volume");
    Vector<u8> index;
    TRY(index.try_resize(block_size));
    for (size_t i = 0; i < m_index_blocks; ++i) {
        auto const& pointer = superblock.bitmap_index[i];
        if (pointer.block < 2 || pointer.block >= m_block_count)
            return Error::from_string_literal("vultureFS: bitmap index block out of range");
        TRY(store.read_blocks(pointer.block, 1, { index.begin(), block_size }));
        if (crc32c({ index.begin(), block_size }) != pointer.checksum)
            return Error::from_string_literal("vultureFS: bitmap index block checksum mismatch");
        size_t const here = min(m_bitmap_blocks - i * bitmap_pointers_per_index_block, bitmap_pointers_per_index_block);
        for (size_t j = 0; j < here; ++j) {
            BitmapPointer entry;
            __builtin_memcpy(&entry, index.begin() + j * sizeof(BitmapPointer), sizeof(entry));
            size_t const which = i * bitmap_pointers_per_index_block + j;
            if (entry.block < 2 || entry.block >= m_block_count)
                return Error::from_string_literal("vultureFS: bitmap block out of range");
            Bytes bytes { reinterpret_cast<u8*>(&words[which * words_per_bitmap_block]), block_size };
            TRY(store.read_blocks(entry.block, 1, bytes));
            if (crc32c(bytes) != entry.checksum)
                return Error::from_string_literal("vultureFS: bitmap block checksum mismatch");
        }
    }
    return {};
}

ErrorOr<void> Allocator::load(BlockStore& store, Superblock const& superblock)
{
    TRY(initialize(superblock.block_count));
    TRY(read_bitmap(store, superblock, m_used));
    Vector<u8> index;
    TRY(index.try_resize(block_size));
    for (size_t i = 0; i < m_index_blocks; ++i) {
        m_index_locations[i] = superblock.bitmap_index[i];
        TRY(store.read_blocks(m_index_locations[i].block, 1, { index.begin(), block_size }));
        size_t const here = min(m_bitmap_blocks - i * bitmap_pointers_per_index_block, bitmap_pointers_per_index_block);
        for (size_t j = 0; j < here; ++j) {
            size_t const which = i * bitmap_pointers_per_index_block + j;
            __builtin_memcpy(&m_locations[which], index.begin() + j * sizeof(BitmapPointer), sizeof(BitmapPointer));
            m_dirty[which] = 0;
        }
    }
    for (u64 block = m_block_count; block < m_used.size() * 64; ++block)
        m_used[block / 64] |= 1ull << (block % 64);
    for (size_t i = 0; i < m_used.size(); ++i)
        m_committed[i] = m_used[i];
    count_blocks();
    if (m_used_count != m_block_count - superblock.free_blocks)
        return Error::from_string_literal("vultureFS: free block count does not match the bitmap");
    if (!is_used(0) || !is_used(1))
        return Error::from_string_literal("vultureFS: superblocks are not marked in use");
    return {};
}

void Allocator::count_blocks()
{
    u64 used = 0;
    u64 held = 0;
    bool const has_held = !m_held.is_empty();
    for (size_t i = 0; i < m_used.size(); ++i) {
        used += static_cast<u64>(__builtin_popcountll(m_used[i]));
        if (has_held)
            held += static_cast<u64>(__builtin_popcountll(m_held[i] & ~m_used[i]));
    }
    m_used_count = used - (m_used.size() * 64 - m_block_count);
    m_held_count = held;
}

ErrorOr<void> Allocator::clear_snapshots()
{
    m_held.clear();
    count_blocks();
    return {};
}

ErrorOr<void> Allocator::add_snapshot(BlockStore& store, Superblock const& superblock)
{
    Vector<u64> words;
    TRY(words.try_resize(m_used.size()));
    TRY(read_bitmap(store, superblock, words));
    if (m_held.is_empty()) {
        m_held = move(words);
    } else {
        for (size_t i = 0; i < words.size(); ++i)
            m_held[i] |= words[i];
    }
    // Bits past the end say nothing here, and shouldn't count.
    for (u64 block = m_block_count; block < m_used.size() * 64; ++block)
        m_held[block / 64] &= ~(1ull << (block % 64));
    count_blocks();
    // What was only waiting for a checkpoint may now be waiting for this as well.
    u64 pending = 0;
    for (size_t i = 0; i < m_used.size(); ++i)
        pending += static_cast<u64>(__builtin_popcountll(m_committed[i] & ~m_used[i] & ~m_held[i]));
    m_pending_frees = pending;
    return {};
}

ErrorOr<void> Allocator::go_back_to(BlockStore& store, Superblock const& superblock)
{
    TRY(read_bitmap(store, superblock, m_used));
    for (u64 block = m_block_count; block < m_used.size() * 64; ++block)
        m_used[block / 64] |= 1ull << (block % 64);
    Vector<u8> index;
    TRY(index.try_resize(block_size));
    for (size_t i = 0; i < m_index_blocks; ++i) {
        m_index_locations[i] = superblock.bitmap_index[i];
        TRY(store.read_blocks(m_index_locations[i].block, 1, { index.begin(), block_size }));
        size_t const here = min(m_bitmap_blocks - i * bitmap_pointers_per_index_block, bitmap_pointers_per_index_block);
        for (size_t j = 0; j < here; ++j) {
            size_t const which = i * bitmap_pointers_per_index_block + j;
            __builtin_memcpy(&m_locations[which], index.begin() + j * sizeof(BitmapPointer), sizeof(BitmapPointer));
            m_dirty[which] = 1;
        }
    }
    count_blocks();
    u64 pending = 0;
    bool const has_held = !m_held.is_empty();
    for (size_t i = 0; i < m_used.size(); ++i)
        pending += static_cast<u64>(__builtin_popcountll(m_committed[i] & ~m_used[i] & (has_held ? ~m_held[i] : ~0ull)));
    m_pending_frees = pending;
    return {};
}

void Allocator::set_used(BlockIndex block, bool used)
{
    VERIFY(block < m_block_count);
    u64 mask = 1ull << (block % 64);
    auto& word = m_used[block / 64];
    VERIFY(static_cast<bool>(word & mask) != used);
    if (used) {
        word |= mask;
        ++m_used_count;
    } else {
        word &= ~mask;
        --m_used_count;
    }
    m_dirty[block / blocks_per_bitmap_block] = 1;
}

ErrorOr<BlockIndex> Allocator::allocate()
{
    auto run = TRY(allocate_run(1, m_cursor));
    return run.first;
}

ErrorOr<BlockRun> Allocator::allocate_run(u64 count, BlockIndex near)
{
    VERIFY(count > 0);
    size_t words = m_used.size();
    size_t start_word = (near < m_block_count ? near : 0) / 64;
    bool const has_held = !m_held.is_empty();
    for (size_t step = 0; step <= words; ++step) {
        size_t word_index = (start_word + step) % words;
        u64 taken = m_used[word_index] | m_committed[word_index] | (has_held ? m_held[word_index] : 0);
        if (taken == ~0ull)
            continue;
        BlockIndex first = word_index * 64 + static_cast<u64>(__builtin_ctzll(~taken));
        u64 length = 0;
        while (length < count && first + length < m_block_count && is_available(first + length))
            ++length;
        for (u64 i = 0; i < length; ++i)
            set_used(first + i, true);
        m_cursor = first + length;
        return BlockRun { first, length };
    }
    return Error::from_errno(enospc);
}

void Allocator::free(BlockIndex first, u64 count)
{
    for (u64 i = 0; i < count; ++i) {
        set_used(first + i, false);
        if (is_held(first + i))
            ++m_held_count;
        else if (bit(m_committed, first + i))
            ++m_pending_frees;
    }
}

ErrorOr<void> Allocator::write(BlockStore& store, Superblock& superblock)
{
    // Moving a bitmap block changes bits, maybe in another bitmap block,
    // which then moves too; and something it points to having moved, an index block
    // moves as well. Each moves at most once, so this ends.
    Vector<u8> moved;
    TRY(moved.try_resize(m_bitmap_blocks));
    for (size_t i = 0; i < m_bitmap_blocks; ++i)
        moved[i] = 0;
    Vector<u8> index_moved;
    TRY(index_moved.try_resize(m_index_blocks));
    for (size_t i = 0; i < m_index_blocks; ++i)
        index_moved[i] = 0;
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < m_bitmap_blocks; ++i) {
            if (!m_dirty[i] || moved[i])
                continue;
            if (m_locations[i].block != 0)
                free(m_locations[i].block, 1);
            m_locations[i].block = TRY(allocate());
            moved[i] = 1;
            progress = true;
            size_t const index = i / bitmap_pointers_per_index_block;
            if (!index_moved[index]) {
                if (m_index_locations[index].block != 0)
                    free(m_index_locations[index].block, 1);
                m_index_locations[index].block = TRY(allocate());
                index_moved[index] = 1;
            }
        }
    }
    for (size_t i = 0; i < m_bitmap_blocks; ++i) {
        if (!moved[i])
            continue;
        ReadonlyBytes bytes { reinterpret_cast<u8 const*>(&m_used[i * words_per_bitmap_block]), block_size };
        m_locations[i].checksum = crc32c(bytes);
        TRY(store.write_blocks(m_locations[i].block, 1, bytes));
    }
    Vector<u8> index;
    TRY(index.try_resize(block_size));
    for (size_t i = 0; i < m_index_blocks; ++i) {
        if (!index_moved[i])
            continue;
        __builtin_memset(index.begin(), 0, block_size);
        size_t const here = min(m_bitmap_blocks - i * bitmap_pointers_per_index_block, bitmap_pointers_per_index_block);
        __builtin_memcpy(index.begin(), &m_locations[i * bitmap_pointers_per_index_block], here * sizeof(BitmapPointer));
        m_index_locations[i].checksum = crc32c({ index.begin(), block_size });
        TRY(store.write_blocks(m_index_locations[i].block, 1, { index.begin(), block_size }));
    }
    superblock.bitmap_block_count = static_cast<u32>(m_bitmap_blocks);
    for (size_t i = 0; i < max_bitmap_index_blocks; ++i)
        superblock.bitmap_index[i] = i < m_index_blocks ? m_index_locations[i] : BitmapPointer {};
    superblock.free_blocks = m_block_count - m_used_count;
    return {};
}

void Allocator::commit()
{
    for (size_t i = 0; i < m_used.size(); ++i)
        m_committed[i] = m_used[i];
    for (size_t i = 0; i < m_bitmap_blocks; ++i)
        m_dirty[i] = 0;
    m_pending_frees = 0;
}

}
