/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibDatabase/BTree.h>

namespace Database {

namespace {

// A page: its header, then two bytes for where each cell is, in key order, and the cells from the end of the page back.
constexpr u8 kind_leaf = 1;
constexpr u8 kind_interior = 2;
constexpr size_t page_header = 12;
constexpr size_t offset_count = 2;
constexpr size_t offset_cells_start = 4;
constexpr size_t offset_fragmented = 6;
constexpr size_t offset_rightmost = 8;

// A leaf's cell: key length (2), value length (4), the key, then the value, or the page it starts on if it's long.
// An interior page's: child (4), key length (2), the key: what's under that child is less than the key.
constexpr size_t leaf_cell_header = 6;
constexpr size_t interior_cell_header = 6;
constexpr size_t longest_inline_value = 1800;

// A page of a long value: the next (0 for none), then as much as fits.
constexpr size_t overflow_header = 4;
constexpr size_t overflow_capacity = page_size - overflow_header;

constexpr Error damaged = Error::from_string_literal("database: a tree's page is damaged");

u16 read16(u8 const* bytes, size_t offset)
{
    u16 value;
    __builtin_memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

u32 read32(u8 const* bytes, size_t offset)
{
    u32 value;
    __builtin_memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

void write16(u8* bytes, size_t offset, size_t value)
{
    u16 narrow = static_cast<u16>(value);
    __builtin_memcpy(bytes + offset, &narrow, sizeof(narrow));
}

void write32(u8* bytes, size_t offset, u32 value)
{
    __builtin_memcpy(bytes + offset, &value, sizeof(value));
}

u8 kind_of(u8 const* page) { return page[0]; }
u32 count_of(u8 const* page) { return read16(page, offset_count); }
u32 rightmost_of(u8 const* page) { return read32(page, offset_rightmost); }
void set_rightmost(u8* page, u32 child) { write32(page, offset_rightmost, child); }
size_t cell_offset(u8 const* page, u32 index) { return read16(page, page_header + 2 * index); }

void initialize_page(u8* page, u8 kind)
{
    __builtin_memset(page, 0, page_size);
    page[0] = kind;
    write16(page, offset_cells_start, page_size);
}

size_t leaf_cell_size(u8 const* cell)
{
    size_t key = read16(cell, 0);
    u32 value = read32(cell, 2);
    return leaf_cell_header + key + (value <= longest_inline_value ? value : 4);
}

size_t interior_cell_size(u8 const* cell)
{
    return interior_cell_header + read16(cell, 4);
}

size_t cell_size(u8 const* page, u32 index)
{
    u8 const* cell = page + cell_offset(page, index);
    return kind_of(page) == kind_leaf ? leaf_cell_size(cell) : interior_cell_size(cell);
}

ReadonlyBytes key_of_cell(u8 const* page, u32 index)
{
    u8 const* cell = page + cell_offset(page, index);
    if (kind_of(page) == kind_leaf)
        return { cell + leaf_cell_header, read16(cell, 0) };
    return { cell + interior_cell_header, read16(cell, 4) };
}

u32 child_of_cell(u8 const* page, u32 index)
{
    return read32(page + cell_offset(page, index), 0);
}

u32 child_at(u8 const* page, u32 index)
{
    return index < count_of(page) ? child_of_cell(page, index) : rightmost_of(page);
}

void set_child_at(u8* page, u32 index, u32 child)
{
    if (index < count_of(page))
        write32(page + cell_offset(page, index), 0, child);
    else
        set_rightmost(page, child);
}

int compare_keys(ReadonlyBytes a, ReadonlyBytes b)
{
    size_t common = min(a.size(), b.size());
    int result = common ? __builtin_memcmp(a.data(), b.data(), common) : 0;
    if (result != 0)
        return result;
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

// The first cell whose key isn't less than this one.
u32 lower_bound(u8 const* page, ReadonlyBytes key, bool* found = nullptr)
{
    u32 low = 0;
    u32 high = count_of(page);
    while (low < high) {
        u32 middle = low + (high - low) / 2;
        if (compare_keys(key_of_cell(page, middle), key) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (found)
        *found = low < count_of(page) && compare_keys(key_of_cell(page, low), key) == 0;
    return low;
}

// Which child a key is under: the first whose limit is more than the key.
u32 child_index_for(u8 const* page, ReadonlyBytes key)
{
    u32 low = 0;
    u32 high = count_of(page);
    while (low < high) {
        u32 middle = low + (high - low) / 2;
        if (compare_keys(key_of_cell(page, middle), key) <= 0)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

size_t free_contiguous(u8 const* page)
{
    return read16(page, offset_cells_start) == 0 ? 0 : read16(page, offset_cells_start) - (page_header + 2 * count_of(page));
}

// A cells_start of 8192 doesn't fit in 13 bits, but does in 16, so a new page's 8192 is as it reads.
size_t free_total(u8 const* page)
{
    return free_contiguous(page) + read16(page, offset_fragmented);
}

void defragment(u8* page)
{
    u8 copy[page_size];
    __builtin_memcpy(copy, page, page_size);
    u32 count = count_of(page);
    size_t top = page_size;
    for (u32 i = 0; i < count; ++i) {
        size_t size = cell_size(copy, i);
        top -= size;
        __builtin_memcpy(page + top, copy + cell_offset(copy, i), size);
        write16(page, page_header + 2 * i, top);
    }
    write16(page, offset_cells_start, top);
    write16(page, offset_fragmented, 0);
}

bool fits(u8 const* page, size_t size)
{
    return free_total(page) >= size + 2;
}

void insert_cell(u8* page, u32 index, ReadonlyBytes cell)
{
    if (free_contiguous(page) < cell.size() + 2)
        defragment(page);
    u32 count = count_of(page);
    size_t top = read16(page, offset_cells_start) - cell.size();
    __builtin_memcpy(page + top, cell.data(), cell.size());
    __builtin_memmove(page + page_header + 2 * (index + 1), page + page_header + 2 * index, 2 * (count - index));
    write16(page, page_header + 2 * index, top);
    write16(page, offset_count, count + 1);
    write16(page, offset_cells_start, top);
}

void remove_cell(u8* page, u32 index)
{
    size_t size = cell_size(page, index);
    u32 count = count_of(page);
    __builtin_memmove(page + page_header + 2 * index, page + page_header + 2 * (index + 1), 2 * (count - index - 1));
    write16(page, offset_count, count - 1);
    write16(page, offset_fragmented, read16(page, offset_fragmented) + size);
}

void make_leaf_cell(Vector<u8>& out, ReadonlyBytes key, ReadonlyBytes value, u32 overflow)
{
    out.clear();
    bool is_inline = value.size() <= longest_inline_value;
    MUST(out.try_ensure_capacity(leaf_cell_header + key.size() + (is_inline ? value.size() : 4)));
    out.append(static_cast<u8>(key.size()));
    out.append(static_cast<u8>(key.size() >> 8));
    u32 const length = static_cast<u32>(value.size());
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<u8>(length >> (i * 8)));
    for (auto byte : key)
        out.append(byte);
    if (is_inline) {
        for (auto byte : value)
            out.append(byte);
    } else {
        for (int i = 0; i < 4; ++i)
            out.append(static_cast<u8>(overflow >> (i * 8)));
    }
}

void make_interior_cell(Vector<u8>& out, u32 child, ReadonlyBytes key)
{
    out.clear();
    MUST(out.try_ensure_capacity(interior_cell_header + key.size()));
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<u8>(child >> (i * 8)));
    out.append(static_cast<u8>(key.size()));
    out.append(static_cast<u8>(key.size() >> 8));
    for (auto byte : key)
        out.append(byte);
}

// Only as much of the right one's first key as tells it from the left one's last.
ReadonlyBytes separator_between(ReadonlyBytes left_last, ReadonlyBytes right_first)
{
    size_t common = 0;
    while (common < left_last.size() && common < right_first.size() && left_last[common] == right_first[common])
        ++common;
    return { right_first.data(), min(common + 1, right_first.size()) };
}

struct Piece {
    u8 const* bytes;
    size_t size;
};

void fill_page(u8* page, u8 kind, Span<Piece const> cells)
{
    initialize_page(page, kind);
    size_t top = page_size;
    for (size_t i = 0; i < cells.size(); ++i) {
        top -= cells[i].size;
        __builtin_memcpy(page + top, cells[i].bytes, cells[i].size);
        write16(page, page_header + 2 * i, top);
    }
    write16(page, offset_count, cells.size());
    write16(page, offset_cells_start, top);
}

}

ErrorOr<u32> BTree::create(Pager& pager)
{
    auto page = TRY(pager.allocate());
    initialize_page(page.data(), kind_leaf);
    return page.number();
}

ErrorOr<u32> BTree::write_overflow(ReadonlyBytes value)
{
    u32 first = 0;
    PageRef previous;
    size_t done = 0;
    while (done < value.size()) {
        auto page = TRY(m_pager.allocate());
        size_t piece = min(value.size() - done, overflow_capacity);
        __builtin_memcpy(page.data() + overflow_header, value.data() + done, piece);
        if (previous)
            write32(previous.data(), 0, page.number());
        else
            first = page.number();
        previous = move(page);
        done += piece;
    }
    return first;
}

ErrorOr<void> BTree::free_overflow(u32 first)
{
    while (first != 0) {
        auto page = TRY(m_pager.get(first));
        u32 next = read32(page.data(), 0);
        page = PageRef();
        TRY(m_pager.free(first));
        first = next;
    }
    return {};
}

ErrorOr<void> BTree::insert(ReadonlyBytes key, ReadonlyBytes value)
{
    if (key.size() > longest_key)
        return Error::from_string_literal("database: that's too long for an index");
    Vector<PathEntry> path;
    auto page = TRY(m_pager.get(m_root));
    while (kind_of(page.data()) == kind_interior) {
        u32 child = child_index_for(page.data(), key);
        path.append(PathEntry { page.number(), child });
        page = TRY(m_pager.get(child_at(page.data(), child)));
    }
    if (kind_of(page.data()) != kind_leaf)
        return damaged;

    bool found = false;
    u32 index = lower_bound(page.data(), key, &found);
    TRY(m_pager.make_writable(page));
    if (found) {
        u8 const* cell = page.data() + cell_offset(page.data(), index);
        u32 old_length = read32(cell, 2);
        if (old_length > longest_inline_value)
            TRY(free_overflow(read32(cell, leaf_cell_header + read16(cell, 0))));
        remove_cell(page.data(), index);
    }

    u32 overflow = 0;
    if (value.size() > longest_inline_value)
        overflow = TRY(write_overflow(value));
    Vector<u8> cell;
    make_leaf_cell(cell, key, value, overflow);
    if (fits(page.data(), cell.size())) {
        insert_cell(page.data(), index, cell.span());
        return {};
    }
    return split_leaf(page, path, index, cell.span());
}

ErrorOr<void> BTree::split_leaf(PageRef& page, Vector<PathEntry>& path, u32 index, ReadonlyBytes new_cell)
{
    u8 old[page_size];
    __builtin_memcpy(old, page.data(), page_size);
    u32 count = count_of(old);

    Vector<Piece> pieces;
    TRY(pieces.try_ensure_capacity(count + 1));
    size_t total = 0;
    for (u32 i = 0; i <= count; ++i) {
        if (i == index) {
            pieces.append(Piece { new_cell.data(), new_cell.size() });
            total += new_cell.size() + 2;
        }
        if (i < count) {
            size_t size = cell_size(old, i);
            pieces.append(Piece { old + cell_offset(old, i), size });
            total += size + 2;
        }
    }

    // Going to the end, as with rows being added in order: all the old ones stay, and the page ends up full.
    size_t split = pieces.size() - 1;
    if (index != count) {
        size_t used = 0;
        split = 1;
        for (size_t i = 0; i + 1 < pieces.size(); ++i) {
            used += pieces[i].size + 2;
            split = i + 1;
            if (used >= total / 2)
                break;
        }
    }

    auto left_span = Span<Piece const>(pieces.data(), split);
    auto right_span = Span<Piece const>(pieces.data() + split, pieces.size() - split);
    auto key_of_piece = [](Piece const& piece) { return ReadonlyBytes(piece.bytes + leaf_cell_header, read16(piece.bytes, 0)); };

    Vector<u8> separator_bytes;
    auto separator = separator_between(key_of_piece(pieces[split - 1]), key_of_piece(pieces[split]));
    for (auto byte : separator)
        separator_bytes.append(byte);

    if (path.is_empty()) {
        // The root stays where it is, and takes on the two as children.
        auto left = TRY(m_pager.allocate());
        auto right = TRY(m_pager.allocate());
        fill_page(left.data(), kind_leaf, left_span);
        fill_page(right.data(), kind_leaf, right_span);
        initialize_page(page.data(), kind_interior);
        Vector<u8> cell;
        make_interior_cell(cell, left.number(), separator_bytes.span());
        insert_cell(page.data(), 0, cell.span());
        set_rightmost(page.data(), right.number());
        return {};
    }

    auto right = TRY(m_pager.allocate());
    fill_page(page.data(), kind_leaf, left_span);
    fill_page(right.data(), kind_leaf, right_span);
    return insert_into_parent(path, separator_bytes.span(), right.number());
}

ErrorOr<void> BTree::insert_into_parent(Vector<PathEntry>& path, ReadonlyBytes separator, u32 right)
{
    auto entry = path.last();
    path.remove(path.size() - 1);
    auto parent = TRY(m_pager.get(entry.page));
    TRY(m_pager.make_writable(parent));
    u32 left = child_at(parent.data(), entry.child);

    Vector<u8> cell;
    make_interior_cell(cell, left, separator);
    if (fits(parent.data(), cell.size())) {
        insert_cell(parent.data(), entry.child, cell.span());
        set_child_at(parent.data(), entry.child + 1, right);
        return {};
    }
    return split_interior(parent, path, entry.child, cell.span(), right);
}

ErrorOr<void> BTree::split_interior(PageRef& page, Vector<PathEntry>& path, u32 index, ReadonlyBytes new_cell, u32 pointer_after)
{
    u8 old[page_size];
    __builtin_memcpy(old, page.data(), page_size);
    u32 count = count_of(old);

    // The cells as they'd be, and what comes last, with the new pointer where the old left one was.
    Vector<Vector<u8>> cells;
    TRY(cells.try_ensure_capacity(count + 1));
    u32 rightmost = rightmost_of(old);
    size_t total = 0;
    for (u32 i = 0; i <= count; ++i) {
        if (i == index) {
            Vector<u8> copy;
            for (auto byte : new_cell)
                copy.append(byte);
            total += copy.size() + 2;
            cells.append(move(copy));
        }
        if (i < count) {
            size_t size = cell_size(old, i);
            Vector<u8> copy;
            TRY(copy.try_ensure_capacity(size));
            for (size_t b = 0; b < size; ++b)
                copy.append(old[cell_offset(old, i) + b]);
            // The cell after the new one now goes down to what split off.
            if (i == index)
                write32(copy.data(), 0, pointer_after);
            total += copy.size() + 2;
            cells.append(move(copy));
        }
    }
    if (index == count)
        rightmost = pointer_after;

    size_t middle = 1;
    size_t used = 0;
    for (size_t i = 0; i + 1 < cells.size(); ++i) {
        used += cells[i].size() + 2;
        middle = i;
        if (used >= total / 2)
            break;
    }
    if (middle == 0)
        middle = 1;
    if (middle >= cells.size() - 1)
        middle = cells.size() - 2;

    Vector<Piece> left_pieces;
    Vector<Piece> right_pieces;
    for (size_t i = 0; i < middle; ++i)
        left_pieces.append(Piece { cells[i].data(), cells[i].size() });
    for (size_t i = middle + 1; i < cells.size(); ++i)
        right_pieces.append(Piece { cells[i].data(), cells[i].size() });
    u32 left_rightmost = read32(cells[middle].data(), 0);
    Vector<u8> separator_bytes;
    for (size_t i = interior_cell_header; i < cells[middle].size(); ++i)
        separator_bytes.append(cells[middle][i]);

    if (path.is_empty()) {
        auto left = TRY(m_pager.allocate());
        auto right = TRY(m_pager.allocate());
        fill_page(left.data(), kind_interior, left_pieces.span());
        set_rightmost(left.data(), left_rightmost);
        fill_page(right.data(), kind_interior, right_pieces.span());
        set_rightmost(right.data(), rightmost);
        initialize_page(page.data(), kind_interior);
        Vector<u8> cell;
        make_interior_cell(cell, left.number(), separator_bytes.span());
        insert_cell(page.data(), 0, cell.span());
        set_rightmost(page.data(), right.number());
        return {};
    }

    auto right = TRY(m_pager.allocate());
    fill_page(page.data(), kind_interior, left_pieces.span());
    set_rightmost(page.data(), left_rightmost);
    fill_page(right.data(), kind_interior, right_pieces.span());
    set_rightmost(right.data(), rightmost);
    return insert_into_parent(path, separator_bytes.span(), right.number());
}

ErrorOr<bool> BTree::remove(ReadonlyBytes key)
{
    Vector<PathEntry> path;
    auto page = TRY(m_pager.get(m_root));
    while (kind_of(page.data()) == kind_interior) {
        u32 child = child_index_for(page.data(), key);
        path.append(PathEntry { page.number(), child });
        page = TRY(m_pager.get(child_at(page.data(), child)));
    }
    bool found = false;
    u32 index = lower_bound(page.data(), key, &found);
    if (!found)
        return false;
    TRY(m_pager.make_writable(page));
    u8 const* cell = page.data() + cell_offset(page.data(), index);
    if (read32(cell, 2) > longest_inline_value)
        TRY(free_overflow(read32(cell, leaf_cell_header + read16(cell, 0))));
    remove_cell(page.data(), index);
    if (count_of(page.data()) == 0 && !path.is_empty())
        TRY(collapse(path, move(page)));
    return true;
}

ErrorOr<void> BTree::collapse(Vector<PathEntry>& path, PageRef emptied)
{
    u32 number = emptied.number();
    emptied = PageRef();
    TRY(m_pager.free(number));
    while (!path.is_empty()) {
        auto entry = path.last();
        path.remove(path.size() - 1);
        auto parent = TRY(m_pager.get(entry.page));
        TRY(m_pager.make_writable(parent));
        u32 count = count_of(parent.data());
        if (entry.child < count) {
            remove_cell(parent.data(), entry.child);
        } else if (count > 0) {
            set_rightmost(parent.data(), child_of_cell(parent.data(), count - 1));
            remove_cell(parent.data(), count - 1);
        } else {
            // Its only child's gone, and it's as empty as that was.
            if (path.is_empty()) {
                initialize_page(parent.data(), kind_leaf);
                return {};
            }
            u32 parent_number = parent.number();
            parent = PageRef();
            TRY(m_pager.free(parent_number));
            continue;
        }
        break;
    }
    return shrink_root();
}

ErrorOr<void> BTree::shrink_root()
{
    for (;;) {
        auto root = TRY(m_pager.get(m_root));
        if (kind_of(root.data()) != kind_interior || count_of(root.data()) != 0)
            return {};
        u32 only_child = rightmost_of(root.data());
        auto child = TRY(m_pager.get(only_child));
        TRY(m_pager.make_writable(root));
        __builtin_memcpy(root.data(), child.data(), page_size);
        child = PageRef();
        TRY(m_pager.free(only_child));
    }
}

ErrorOr<bool> BTree::get(ReadonlyBytes key, Vector<u8>& value)
{
    auto cursor = TRY(seek(key));
    if (!cursor.is_valid() || compare_keys(cursor.key(), key) != 0)
        return false;
    TRY(cursor.value(value));
    return true;
}

ErrorOr<void> BTree::destroy_page(u32 number)
{
    auto page = TRY(m_pager.get(number));
    u32 count = count_of(page.data());
    if (kind_of(page.data()) == kind_interior) {
        for (u32 i = 0; i <= count; ++i)
            TRY(destroy_page(child_at(page.data(), i)));
    } else {
        for (u32 i = 0; i < count; ++i) {
            u8 const* cell = page.data() + cell_offset(page.data(), i);
            if (read32(cell, 2) > longest_inline_value)
                TRY(free_overflow(read32(cell, leaf_cell_header + read16(cell, 0))));
        }
    }
    page = PageRef();
    return m_pager.free(number);
}

ErrorOr<void> BTree::destroy()
{
    return destroy_page(m_root);
}

ErrorOr<u64> BTree::check_page(u32 number, ReadonlyBytes lower, bool has_lower, ReadonlyBytes upper, bool has_upper, u32 depth)
{
    if (depth > 64)
        return Error::from_string_literal("database: a tree goes round in a circle");
    auto page = TRY(m_pager.get(number));
    u8 const* bytes = page.data();
    u32 count = count_of(bytes);
    if (kind_of(bytes) != kind_leaf && kind_of(bytes) != kind_interior)
        return damaged;
    for (u32 i = 0; i < count; ++i) {
        size_t offset = cell_offset(bytes, i);
        if (offset < page_header + 2 * count || offset >= page_size || offset + cell_size(bytes, i) > page_size)
            return damaged;
        auto key = key_of_cell(bytes, i);
        if (i > 0 && compare_keys(key_of_cell(bytes, i - 1), key) >= 0)
            return Error::from_string_literal("database: a page's keys are out of order");
        if (has_lower && compare_keys(key, lower) < 0)
            return Error::from_string_literal("database: a key is below where it belongs");
        if (has_upper && compare_keys(key, upper) >= 0)
            return Error::from_string_literal("database: a key is above where it belongs");
    }
    if (kind_of(bytes) == kind_leaf)
        return count;
    u64 total = 0;
    for (u32 i = 0; i <= count; ++i) {
        bool child_has_lower = i > 0 || has_lower;
        auto child_lower = i > 0 ? key_of_cell(bytes, i - 1) : lower;
        bool child_has_upper = i < count || has_upper;
        auto child_upper = i < count ? key_of_cell(bytes, i) : upper;
        total += TRY(check_page(child_at(bytes, i), child_lower, child_has_lower, child_upper, child_has_upper, depth + 1));
    }
    return total;
}

ErrorOr<u64> BTree::check()
{
    return check_page(m_root, {}, false, {}, false, 0);
}

ReadonlyBytes BTree::Cursor::key() const
{
    auto const& level = m_path.last();
    return key_of_cell(level.page.data(), level.index);
}

ErrorOr<void> BTree::Cursor::value(Vector<u8>& out) const
{
    auto const& level = m_path.last();
    u8 const* cell = level.page.data() + cell_offset(level.page.data(), level.index);
    size_t key_length = read16(cell, 0);
    u32 length = read32(cell, 2);
    out.clear();
    TRY(out.try_ensure_capacity(length));
    if (length <= longest_inline_value) {
        for (size_t i = 0; i < length; ++i)
            out.append(cell[leaf_cell_header + key_length + i]);
        return {};
    }
    u32 next = read32(cell, leaf_cell_header + key_length);
    size_t remaining = length;
    while (remaining > 0) {
        if (next == 0)
            return damaged;
        auto page = TRY(m_pager->get(next));
        size_t piece = min(remaining, overflow_capacity);
        for (size_t i = 0; i < piece; ++i)
            out.append(page.data()[overflow_header + i]);
        remaining -= piece;
        next = read32(page.data(), 0);
    }
    return {};
}

ErrorOr<void> BTree::Cursor::next()
{
    auto& leaf = m_path.last();
    if (++leaf.index < count_of(leaf.page.data()))
        return {};
    // Up to the first level with something further along, then all the way down its left side.
    m_path.remove(m_path.size() - 1);
    while (!m_path.is_empty()) {
        auto& level = m_path.last();
        if (level.index < count_of(level.page.data())) {
            ++level.index;
            for (;;) {
                auto child = TRY(m_pager->get(child_at(m_path.last().page.data(), m_path.last().index)));
                bool is_leaf = kind_of(child.data()) == kind_leaf;
                m_path.append(Level { move(child), 0 });
                if (is_leaf)
                    return {};
            }
        }
        m_path.remove(m_path.size() - 1);
    }
    return {};
}

ErrorOr<void> BTree::Cursor::previous()
{
    auto& leaf = m_path.last();
    if (leaf.index > 0) {
        --leaf.index;
        return {};
    }
    m_path.remove(m_path.size() - 1);
    while (!m_path.is_empty()) {
        auto& level = m_path.last();
        if (level.index > 0) {
            --level.index;
            for (;;) {
                auto child = TRY(m_pager->get(child_at(m_path.last().page.data(), m_path.last().index)));
                bool is_leaf = kind_of(child.data()) == kind_leaf;
                u32 count = count_of(child.data());
                u32 index = is_leaf ? (count > 0 ? count - 1 : 0) : count;
                m_path.append(Level { move(child), index });
                if (is_leaf)
                    return {};
            }
        }
        m_path.remove(m_path.size() - 1);
    }
    return {};
}

ErrorOr<BTree::Cursor> BTree::first()
{
    Cursor cursor(m_pager);
    auto page = TRY(m_pager.get(m_root));
    for (;;) {
        bool is_leaf = kind_of(page.data()) == kind_leaf;
        u32 count = count_of(page.data());
        u32 child = is_leaf ? 0 : child_at(page.data(), 0);
        cursor.m_path.append(Cursor::Level { page, 0 });
        if (is_leaf) {
            if (count == 0)
                cursor.m_path.clear();
            return cursor;
        }
        page = TRY(m_pager.get(child));
    }
}

ErrorOr<BTree::Cursor> BTree::last()
{
    Cursor cursor(m_pager);
    auto page = TRY(m_pager.get(m_root));
    for (;;) {
        bool is_leaf = kind_of(page.data()) == kind_leaf;
        u32 count = count_of(page.data());
        if (is_leaf) {
            if (count == 0) {
                cursor.m_path.clear();
                return cursor;
            }
            cursor.m_path.append(Cursor::Level { page, count - 1 });
            return cursor;
        }
        u32 child = child_at(page.data(), count);
        cursor.m_path.append(Cursor::Level { page, count });
        page = TRY(m_pager.get(child));
    }
}

ErrorOr<BTree::Cursor> BTree::seek(ReadonlyBytes key)
{
    Cursor cursor(m_pager);
    auto page = TRY(m_pager.get(m_root));
    while (kind_of(page.data()) == kind_interior) {
        u32 child = child_index_for(page.data(), key);
        u32 child_page = child_at(page.data(), child);
        cursor.m_path.append(Cursor::Level { page, child });
        page = TRY(m_pager.get(child_page));
    }
    u32 index = lower_bound(page.data(), key);
    u32 count = count_of(page.data());
    if (count == 0 && cursor.m_path.is_empty())
        return cursor;
    // Past everything in this leaf: where it goes on is the first of the next.
    if (index >= count) {
        cursor.m_path.append(Cursor::Level { page, count > 0 ? count - 1 : 0 });
        TRY(cursor.next());
        return cursor;
    }
    cursor.m_path.append(Cursor::Level { page, index });
    return cursor;
}

}
