/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/IntrusiveList.h>
#include <LibBase/Optional.h>
#include <LibBase/Span.h>

namespace Kernel {

// Hands out the page frames of one physically contiguous range in blocks of
// 2^order pages. A block is aligned to its own size in the physical address
// space, so its "buddy" (the other half of the next larger block) is found
// by flipping one bit of its frame number. Freeing a block merges it with
// its buddy whenever that is free too, which keeps large blocks available.
//
// Free blocks are kept on per-order lists threaded through the free memory
// itself; the only other state is two bytes per page. Not thread-safe: the
// caller provides locking. Hardware independent, so it is unit-tested on
// the host as well.
class BuddyAllocator {
public:
    static constexpr unsigned max_order = 10; // 4 MiB blocks

    static constexpr size_t metadata_size(size_t page_count) { return page_count * sizeof(PageState); }

    BuddyAllocator() = default;
    BuddyAllocator(BuddyAllocator const&) = delete;
    BuddyAllocator& operator=(BuddyAllocator const&) = delete;

    // Every page starts out reserved; add_free_range() makes pages available.
    // `metadata` holds metadata_size(page_count) bytes. The allocator reads
    // and writes free memory at physical address + `access_offset` (the
    // kernel's direct map, or a buffer in host tests).
    void initialize(PhysicalAddress base, size_t page_count, Bytes metadata, FlatPtr access_offset);

    // Makes reserved pages available. The range must lie inside this allocator.
    void add_free_range(PhysicalAddress, size_t page_count);

    // A block of 2^order pages carrying `tag`, a number the caller chooses
    // to record what the block is used for.
    [[nodiscard]] Optional<PhysicalAddress> allocate(unsigned order, u8 tag);

    // Frees a block that allocate() returned and returns its order.
    unsigned free(PhysicalAddress);

    [[nodiscard]] bool contains(PhysicalAddress) const;

    // The order and tag of the allocated block starting at an address, or
    // nothing if no allocated block starts there.
    [[nodiscard]] Optional<unsigned> block_order(PhysicalAddress) const;
    [[nodiscard]] Optional<u8> block_tag(PhysicalAddress) const;
    void set_block_tag(PhysicalAddress, u8);

    [[nodiscard]] PhysicalAddress base() const { return PhysicalAddress(m_base_frame << page_shift); }
    [[nodiscard]] size_t page_count() const { return m_page_count; }
    [[nodiscard]] size_t free_pages() const { return m_free_pages; }
    [[nodiscard]] size_t free_blocks(unsigned order) const { return m_free_lists[order].size(); }

private:
    enum class Kind : u8 {
        Reserved = 0, // Not managed: firmware, the kernel, holes.
        Tail = 1, // Inside a block, but not its first page.
        Free = 2, // First page of a free block.
        Allocated = 3, // First page of an allocated block.
    };

    struct PageState {
        u8 flags; // Kind in bits 4-5, order in bits 0-3.
        u8 tag;

        [[nodiscard]] Kind kind() const { return static_cast<Kind>(flags >> 4); }
        [[nodiscard]] unsigned order() const { return flags & 0xf; }
        void set(Kind kind, unsigned order) { flags = static_cast<u8>((static_cast<u8>(kind) << 4) | order); }
    };

    // Lives in the first bytes of each free block.
    struct FreeBlock {
        IntrusiveListNode list_node;
    };

    [[nodiscard]] bool is_block_inside(u64 frame, unsigned order) const;
    [[nodiscard]] PageState& state(u64 frame) { return m_states[frame - m_base_frame]; }
    [[nodiscard]] PageState const& state(u64 frame) const { return m_states[frame - m_base_frame]; }
    [[nodiscard]] Optional<u64> frame_of(PhysicalAddress) const;

    void push_free_block(u64 frame, unsigned order);
    void remove_free_block(u64 frame, unsigned order);
    void release_block(u64 frame, unsigned order);

    u64 m_base_frame { 0 };
    size_t m_page_count { 0 };
    size_t m_free_pages { 0 };
    PageState* m_states { nullptr };
    FlatPtr m_access_offset { 0 };
    IntrusiveList<FreeBlock, &FreeBlock::list_node> m_free_lists[max_order + 1];
};

}
