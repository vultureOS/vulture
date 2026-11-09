/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Vector.h>
#include <LibDatabase/Pager.h>

// A B+ tree in a database's pages: keys, in the order of their bytes, each with a value. The tree stays at the page it was
// made at, whatever grows. A value too long for a page has pages of its own.

namespace Database {

class BTree {
public:
    // Longer than this can't be a key: a page has to have room for several.
    static constexpr size_t longest_key = 1800;

    static ErrorOr<u32> create(Pager&);
    BTree(Pager& pager, u32 root)
        : m_pager(pager)
        , m_root(root)
    {
    }

    [[nodiscard]] u32 root() const { return m_root; }

    // Replaces what the key had, if it had anything.
    ErrorOr<void> insert(ReadonlyBytes key, ReadonlyBytes value);
    ErrorOr<bool> remove(ReadonlyBytes key);
    ErrorOr<bool> get(ReadonlyBytes key, Vector<u8>& value);
    // All of its pages, the tree's own among them.
    ErrorOr<void> destroy();
    // That each page is in order, and within its parent's limits. A count of what's in it.
    ErrorOr<u64> check();

    class Cursor {
    public:
        [[nodiscard]] bool is_valid() const { return !m_path.is_empty(); }
        [[nodiscard]] ReadonlyBytes key() const;
        ErrorOr<void> value(Vector<u8>& out) const;
        ErrorOr<void> next();
        ErrorOr<void> previous();

    private:
        friend class BTree;
        struct Level {
            PageRef page;
            u32 index { 0 };
        };
        explicit Cursor(Pager& pager)
            : m_pager(&pager)
        {
        }

        Pager* m_pager;
        Vector<Level> m_path;
    };

    ErrorOr<Cursor> first();
    ErrorOr<Cursor> last();
    // The first key that isn't less than this.
    ErrorOr<Cursor> seek(ReadonlyBytes key);

private:
    struct PathEntry {
        u32 page;
        u32 child;
    };

    ErrorOr<void> insert_into_parent(Vector<PathEntry>& path, ReadonlyBytes separator, u32 right);
    ErrorOr<void> split_leaf(PageRef&, Vector<PathEntry>& path, u32 index, ReadonlyBytes cell);
    ErrorOr<void> split_interior(PageRef&, Vector<PathEntry>& path, u32 index, ReadonlyBytes cell, u32 pointer_after);
    ErrorOr<void> collapse(Vector<PathEntry>& path, PageRef emptied);
    ErrorOr<void> shrink_root();
    ErrorOr<u32> write_overflow(ReadonlyBytes);
    ErrorOr<void> free_overflow(u32 first);
    ErrorOr<void> destroy_page(u32);
    ErrorOr<u64> check_page(u32, ReadonlyBytes lower, bool has_lower, ReadonlyBytes upper, bool has_upper, u32 depth);

    Pager& m_pager;
    u32 m_root;
};

}
