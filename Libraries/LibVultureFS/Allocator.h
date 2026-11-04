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

// Which blocks are in use, one bit per block. Three bitmaps: what is in use
// now, what the last committed checkpoint uses, and what any snapshot uses.
// A block is free to allocate only when none of them has it, so a block freed
// since the last checkpoint is not reused (and overwritten) until a checkpoint
// without it has committed, and a snapshot's are not reused until it's gone.
class Allocator {
public:
    // A fresh volume: everything free except the superblocks.
    ErrorOr<void> initialize(u64 block_count);
    // Reads the bitmap the superblock points to.
    ErrorOr<void> load(BlockStore&, Superblock const&);

    // What a checkpoint's bitmap says, into `words` (which has as many as this has).
    ErrorOr<void> read_bitmap(BlockStore&, Superblock const&, Vector<u64>& words) const;

    [[nodiscard]] u64 block_count() const { return m_block_count; }
    [[nodiscard]] u64 used_count() const { return m_used_count; }
    // Not used by anything, now or by a snapshot.
    [[nodiscard]] u64 free_count() const { return m_block_count - m_used_count - m_held_count; }
    // Only a snapshot is using them.
    [[nodiscard]] u64 held_count() const { return m_held_count; }
    // Free blocks that may be allocated before the next checkpoint.
    [[nodiscard]] u64 available_count() const { return free_count() - m_pending_frees; }
    [[nodiscard]] bool is_used(BlockIndex block) const { return bit(m_used, block); }

    // One block, or ENOSPC.
    ErrorOr<BlockIndex> allocate();
    // Between one and `count` contiguous blocks, searching from `near`.
    ErrorOr<BlockRun> allocate_run(u64 count, BlockIndex near);
    ErrorOr<BlockRun> allocate_run(u64 count) { return allocate_run(count, m_cursor); }
    void free(BlockIndex first, u64 count);

    // Forgets what snapshots hold, ready for adding those that there are: which is
    // only to be done when nothing's relying on those that are going.
    ErrorOr<void> clear_snapshots();
    ErrorOr<void> add_snapshot(BlockStore&, Superblock const&);
    // Now the checkpoint is what it says, and its bitmap will be written anew. What the last
    // committed one uses stays out of bounds until that has been replaced.
    ErrorOr<void> go_back_to(BlockStore&, Superblock const&);

    // Checkpointing: writes the changed bitmap blocks to new places and
    // records them in the superblock. Then, once the superblock is on disk,
    // commit() makes blocks freed before it allocatable.
    ErrorOr<void> write(BlockStore&, Superblock&);
    void commit();

private:
    static bool bit(Vector<u64> const& words, u64 index) { return words[index / 64] & (1ull << (index % 64)); }
    void set_used(BlockIndex, bool);
    [[nodiscard]] bool is_held(BlockIndex block) const { return !m_held.is_empty() && bit(m_held, block); }
    [[nodiscard]] bool is_available(BlockIndex block) const { return !bit(m_used, block) && !bit(m_committed, block) && !is_held(block); }
    void count_blocks();

    u64 m_block_count { 0 };
    u64 m_used_count { 0 };
    u64 m_held_count { 0 };    // By a snapshot, and not in use now.
    u64 m_pending_frees { 0 }; // Freed since the last checkpoint, which uses them.
    BlockIndex m_cursor { 2 };
    Vector<u64> m_used;
    Vector<u64> m_committed;
    Vector<u64> m_held; // Empty when there are no snapshots.
    size_t m_bitmap_blocks { 0 };
    size_t m_index_blocks { 0 };
    Vector<BitmapPointer> m_locations;
    Vector<BitmapPointer> m_index_locations;
    Vector<u8> m_dirty;
};

}
