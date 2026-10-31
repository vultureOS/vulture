/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Vector.h>
#include <LibVultureFS/BlockStore.h>

namespace VultureFS {

struct BlockRun {
    BlockIndex first;
    u64 count;
};

// Which blocks are in use, one bit per block. Two bitmaps: what is in use
// now, and what the last committed checkpoint uses. A block is free to
// allocate only when neither has it, so a block freed since the last
// checkpoint is not reused (and overwritten) until a checkpoint without it
// has committed.
class Allocator {
public:
    // A fresh volume: everything free except the superblocks.
    ErrorOr<void> initialize(u64 block_count);
    // Reads the bitmap the superblock points to.
    ErrorOr<void> load(BlockStore&, Superblock const&);

    [[nodiscard]] u64 block_count() const { return m_block_count; }
    [[nodiscard]] u64 used_count() const { return m_used_count; }
    [[nodiscard]] u64 free_count() const { return m_block_count - m_used_count; }
    // Free blocks that may be allocated before the next checkpoint.
    [[nodiscard]] u64 available_count() const { return free_count() - m_pending_frees; }
    [[nodiscard]] bool is_used(BlockIndex block) const { return bit(m_used, block); }

    // One block, or ENOSPC.
    ErrorOr<BlockIndex> allocate();
    // Between one and `count` contiguous blocks, searching from `near`.
    ErrorOr<BlockRun> allocate_run(u64 count, BlockIndex near);
    ErrorOr<BlockRun> allocate_run(u64 count) { return allocate_run(count, m_cursor); }
    void free(BlockIndex first, u64 count);

    // Checkpointing: writes the changed bitmap blocks to new places and
    // records them in the superblock. Then, once the superblock is on disk,
    // commit() makes blocks freed before it allocatable.
    ErrorOr<void> write(BlockStore&, Superblock&);
    void commit();

private:
    static bool bit(Vector<u64> const& words, u64 index) { return words[index / 64] & (1ull << (index % 64)); }
    void set_used(BlockIndex, bool);
    [[nodiscard]] bool is_available(BlockIndex block) const { return !bit(m_used, block) && !bit(m_committed, block); }

    u64 m_block_count { 0 };
    u64 m_used_count { 0 };
    u64 m_pending_frees { 0 }; // Freed since the last checkpoint, which uses them.
    BlockIndex m_cursor { 2 };
    Vector<u64> m_used;
    Vector<u64> m_committed;
    size_t m_bitmap_blocks { 0 };
    BitmapPointer m_locations[max_bitmap_blocks] {};
    bool m_dirty[max_bitmap_blocks] {};
};

}
