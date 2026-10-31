/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Optional.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Vector.h>
#include <LibVultureFS/Allocator.h>

namespace VultureFS {

enum class Iteration : u8 {
    Continue,
    Stop,
};

// The largest value an item may have; small enough that any overfull node
// splits into two that fit.
constexpr size_t max_value_size = 1200;

struct Item {
    Key key;
    Vector<u8> value;
};

class BTree;

// The copy-on-write B+tree that holds all metadata. Nodes are read on
// demand and changed in memory; a changed node (and so every node above
// it) is "dirty" and gets a new place on disk only when the checkpoint
// writes it. The previous version stays where it was, untouched.
class BTree {
public:
    BTree(BlockStore& store, Allocator& allocator)
        : m_store(store)
        , m_allocator(allocator)
    {
    }
    ~BTree();
    BTree(BTree const&) = delete;
    BTree& operator=(BTree const&) = delete;

    ErrorOr<void> create_empty();
    ErrorOr<void> load(BlockIndex root, u64 generation);

    [[nodiscard]] BlockIndex root_block() const;
    [[nodiscard]] u64 root_generation() const;
    [[nodiscard]] bool is_dirty() const;
    [[nodiscard]] size_t height() const;

    ErrorOr<Optional<Vector<u8>>> find(Key const&);
    // The item with the greatest key not above `key`.
    ErrorOr<Optional<Item>> find_at_or_before(Key const&);
    // Adds the item, or replaces the value of an existing one.
    ErrorOr<void> insert(Key const&, ReadonlyBytes value);
    // Returns whether there was such an item.
    ErrorOr<bool> remove(Key const&);

    // Visits items from the first with a key not below `start`, in order,
    // until `callback(Key const&, ReadonlyBytes value)` returns Stop. The
    // callback must not change the tree.
    template<typename Callback>
    ErrorOr<void> for_each_from(Key const& start, Callback callback)
    {
        bool stop = false;
        auto visit = [](void* context, Key const& key, ReadonlyBytes value) {
            return (*static_cast<Callback*>(context))(key, value);
        };
        return walk(*m_root, start, visit, &callback, stop);
    }

    // Writes every dirty node to a newly allocated block.
    ErrorOr<void> write(u64 generation);
    // Forgets clean nodes kept in memory, if there are many.
    void trim_cache(size_t keep_nodes);
    [[nodiscard]] size_t loaded_nodes() const { return m_loaded_nodes; }

    struct Node;

private:
    struct Split;
    using Visitor = Iteration (*)(void* context, Key const&, ReadonlyBytes);

    ErrorOr<NonnullOwnPtr<Node>> read_node(BlockIndex, u64 generation, u16 level);
    ErrorOr<Node*> child(Node&, size_t index);
    ErrorOr<Optional<Split>> insert_into(Node&, Key const&, ReadonlyBytes value);
    ErrorOr<bool> remove_from(Node&, Key const&);
    ErrorOr<void> rebalance(Node&, size_t index);
    ErrorOr<Optional<Split>> split(Node&);
    ErrorOr<void> walk(Node&, Key const& start, Visitor, void* context, bool& stop);
    ErrorOr<void> write_node(Node&, u64 generation, u8* buffer);
    void discard(OwnPtr<Node>&);
    void drop_children(Node&);

    BlockStore& m_store;
    Allocator& m_allocator;
    OwnPtr<Node> m_root;
    size_t m_loaded_nodes { 0 };
};

struct BTree::Node {
    struct Child {
        Key key;
        BlockIndex block;
        u64 generation;
        OwnPtr<Node> node; // Null until read.
    };

    u16 level { 0 };
    bool dirty { false };
    BlockIndex block { 0 }; // Where the last checkpoint has it; 0 for new.
    u64 generation { 0 };
    size_t bytes { 0 }; // Leaves: room the entries and values need.
    Vector<Item> items;
    Vector<Child> children;

    [[nodiscard]] bool is_leaf() const { return level == 0; }
    [[nodiscard]] size_t count() const { return is_leaf() ? items.size() : children.size(); }
    [[nodiscard]] Key first_key() const { return is_leaf() ? items[0].key : children[0].key; }
    [[nodiscard]] bool is_underfull() const
    {
        return is_leaf() ? bytes < node_capacity / 4 : children.size() < max_internal_entries / 4;
    }
};

}
