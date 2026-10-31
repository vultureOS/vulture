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
    size_t words = m_bitmap_blocks * words_per_bitmap_block;
    TRY(m_used.try_resize(words));
    TRY(m_committed.try_resize(words));
    for (size_t i = 0; i < words; ++i)
        m_used[i] = m_committed[i] = 0;
    // Bits past the end of the volume read as used, so nothing allocates
    // them; they are not counted.
    for (u64 block = block_count; block < words * 64; ++block) {
        m_used[block / 64] |= 1ull << (block % 64);
        m_committed[block / 64] |= 1ull << (block % 64);
    }
    m_used_count = 0;
    m_pending_frees = 0;
    for (size_t i = 0; i < m_bitmap_blocks; ++i) {
        m_locations[i] = {};
        m_dirty[i] = true;
    }
    set_used(0, true);
    set_used(1, true);
    m_committed[0] |= 3;
    return {};
}

ErrorOr<void> Allocator::load(BlockStore& store, Superblock const& superblock)
{
    TRY(initialize(superblock.block_count));
    if (superblock.bitmap_block_count != m_bitmap_blocks)
        return Error::from_string_literal("vultureFS: bitmap size does not match the volume");
    m_used_count = 0;
    for (size_t i = 0; i < m_bitmap_blocks; ++i) {
        m_locations[i] = superblock.bitmap[i];
        m_dirty[i] = false;
        auto* words = &m_used[i * words_per_bitmap_block];
        Bytes bytes { reinterpret_cast<u8*>(words), block_size };
        if (m_locations[i].block < 2 || m_locations[i].block >= m_block_count)
            return Error::from_string_literal("vultureFS: bitmap block out of range");
        TRY(store.read_blocks(m_locations[i].block, 1, bytes));
        if (crc32c(bytes) != m_locations[i].checksum)
            return Error::from_string_literal("vultureFS: bitmap block checksum mismatch");
    }
    for (u64 block = m_block_count; block < m_used.size() * 64; ++block)
        m_used[block / 64] |= 1ull << (block % 64);
    for (size_t i = 0; i < m_used.size(); ++i) {
        m_committed[i] = m_used[i];
        m_used_count += static_cast<u64>(__builtin_popcountll(m_used[i]));
    }
    m_used_count -= m_used.size() * 64 - m_block_count;
    if (m_used_count != m_block_count - superblock.free_blocks)
        return Error::from_string_literal("vultureFS: free block count does not match the bitmap");
    if (!is_used(0) || !is_used(1))
        return Error::from_string_literal("vultureFS: superblocks are not marked in use");
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
    m_dirty[block / blocks_per_bitmap_block] = true;
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
    for (size_t step = 0; step <= words; ++step) {
        size_t word_index = (start_word + step) % words;
        u64 taken = m_used[word_index] | m_committed[word_index];
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
        if (bit(m_committed, first + i))
            ++m_pending_frees;
    }
}

ErrorOr<void> Allocator::write(BlockStore& store, Superblock& superblock)
{
    // Moving a bitmap block changes bits, maybe in another bitmap block,
    // which then moves too. Each moves at most once, so this ends.
    bool moved[max_bitmap_blocks] {};
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < m_bitmap_blocks; ++i) {
            if (!m_dirty[i] || moved[i])
                continue;
            if (m_locations[i].block != 0)
                free(m_locations[i].block, 1);
            m_locations[i].block = TRY(allocate());
            moved[i] = true;
            progress = true;
        }
    }
    for (size_t i = 0; i < m_bitmap_blocks; ++i) {
        if (!moved[i])
            continue;
        ReadonlyBytes bytes { reinterpret_cast<u8 const*>(&m_used[i * words_per_bitmap_block]), block_size };
        m_locations[i].checksum = crc32c(bytes);
        TRY(store.write_blocks(m_locations[i].block, 1, bytes));
    }
    superblock.bitmap_block_count = static_cast<u32>(m_bitmap_blocks);
    for (size_t i = 0; i < max_bitmap_blocks; ++i)
        superblock.bitmap[i] = i < m_bitmap_blocks ? m_locations[i] : BitmapPointer {};
    superblock.free_blocks = free_count();
    return {};
}

void Allocator::commit()
{
    for (size_t i = 0; i < m_used.size(); ++i)
        m_committed[i] = m_used[i];
    for (size_t i = 0; i < m_bitmap_blocks; ++i)
        m_dirty[i] = false;
    m_pending_frees = 0;
}

}
