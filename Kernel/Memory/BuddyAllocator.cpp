/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Memory/BuddyAllocator.h>
#include <LibBase/New.h>

namespace Kernel {

void BuddyAllocator::initialize(PhysicalAddress base, size_t page_count, Bytes metadata, FlatPtr access_offset)
{
    VERIFY(base.is_page_aligned());
    VERIFY(metadata.size() >= metadata_size(page_count));
    m_base_frame = base.page_frame();
    m_page_count = page_count;
    m_free_pages = 0;
    m_states = reinterpret_cast<PageState*>(metadata.data());
    m_access_offset = access_offset;
    __builtin_memset(m_states, 0, metadata_size(page_count));
}

bool BuddyAllocator::is_block_inside(u64 frame, unsigned order) const
{
    return frame >= m_base_frame && frame + (1ull << order) <= m_base_frame + m_page_count;
}

bool BuddyAllocator::contains(PhysicalAddress address) const
{
    u64 frame = address.page_frame();
    return frame >= m_base_frame && frame < m_base_frame + m_page_count;
}

Optional<u64> BuddyAllocator::frame_of(PhysicalAddress address) const
{
    if (!address.is_page_aligned() || !contains(address))
        return {};
    return address.page_frame();
}

void BuddyAllocator::push_free_block(u64 frame, unsigned order)
{
    auto* block = new (reinterpret_cast<void*>((frame << page_shift) + m_access_offset)) FreeBlock;
    // Most recently freed first: its memory is the most likely to be cached.
    m_free_lists[order].prepend(*block);
    state(frame).set(Kind::Free, order);
}

void BuddyAllocator::remove_free_block(u64 frame, unsigned order)
{
    auto* block = reinterpret_cast<FreeBlock*>((frame << page_shift) + m_access_offset);
    m_free_lists[order].remove(*block);
}

// Returns an allocated block to the free lists, merging it with free buddies.
void BuddyAllocator::release_block(u64 frame, unsigned order)
{
    m_free_pages += 1ull << order;
    while (order < max_order) {
        u64 buddy = frame ^ (1ull << order);
        if (!is_block_inside(buddy, order))
            break;
        auto const& buddy_state = state(buddy);
        if (buddy_state.kind() != Kind::Free || buddy_state.order() != order)
            break;
        remove_free_block(buddy, order);
        // The merged block starts at the lower of the two; the other head
        // becomes an ordinary page inside it.
        u64 lower = min(frame, buddy);
        u64 upper = max(frame, buddy);
        state(upper).set(Kind::Tail, 0);
        frame = lower;
        ++order;
    }
    push_free_block(frame, order);
}

void BuddyAllocator::add_free_range(PhysicalAddress start, size_t page_count)
{
    VERIFY(start.is_page_aligned());
    u64 frame = start.page_frame();
    u64 end = frame + page_count;
    VERIFY(frame >= m_base_frame && end <= m_base_frame + m_page_count);

    // Carve the range into the largest aligned blocks that fit.
    while (frame < end) {
        unsigned order = max_order;
        while (order > 0 && ((frame & ((1ull << order) - 1)) != 0 || frame + (1ull << order) > end))
            --order;
        for (u64 page = frame; page < frame + (1ull << order); ++page) {
            VERIFY(state(page).kind() == Kind::Reserved);
            state(page).set(Kind::Tail, 0);
        }
        release_block(frame, order);
        frame += 1ull << order;
    }
}

Optional<PhysicalAddress> BuddyAllocator::allocate(unsigned order, u8 tag)
{
    VERIFY(order <= max_order);
    unsigned found = order;
    while (found <= max_order && m_free_lists[found].is_empty())
        ++found;
    if (found > max_order)
        return {};

    auto* block = m_free_lists[found].take_first();
    u64 frame = (reinterpret_cast<FlatPtr>(block) - m_access_offset) >> page_shift;
    // Split down to the requested size; each split frees the upper half.
    while (found > order) {
        --found;
        push_free_block(frame + (1ull << found), found);
    }
    auto& head = state(frame);
    head.set(Kind::Allocated, order);
    head.tag = tag;
    m_free_pages -= 1ull << order;
    return PhysicalAddress(frame << page_shift);
}

unsigned BuddyAllocator::free(PhysicalAddress address)
{
    auto frame = frame_of(address);
    VERIFY(frame.has_value());
    auto& head = state(frame.value());
    // Catches double frees and pointers into the middle of a block.
    VERIFY(head.kind() == Kind::Allocated);
    unsigned order = head.order();
    head.tag = 0;
    release_block(frame.value(), order);
    return order;
}

Optional<unsigned> BuddyAllocator::block_order(PhysicalAddress address) const
{
    auto frame = frame_of(address);
    if (!frame.has_value() || state(frame.value()).kind() != Kind::Allocated)
        return {};
    return state(frame.value()).order();
}

Optional<u8> BuddyAllocator::block_tag(PhysicalAddress address) const
{
    auto frame = frame_of(address);
    if (!frame.has_value() || state(frame.value()).kind() != Kind::Allocated)
        return {};
    return state(frame.value()).tag;
}

void BuddyAllocator::set_block_tag(PhysicalAddress address, u8 tag)
{
    auto frame = frame_of(address);
    VERIFY(frame.has_value() && state(frame.value()).kind() == Kind::Allocated);
    state(frame.value()).tag = tag;
}

}
