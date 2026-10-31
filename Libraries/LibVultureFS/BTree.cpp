/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Checksum.h>
#include <LibVultureFS/BTree.h>

namespace VultureFS {

static constexpr u16 any_level = 0xffff;

struct BTree::Split {
    Key key;
    NonnullOwnPtr<Node> node;
};

// The first item whose key is not below `key`.
static size_t lower_bound(Vector<Item> const& items, Key const& key)
{
    size_t low = 0, high = items.size();
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (items[middle].key < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

// The child whose subtree would hold `key`.
template<typename Children>
static size_t child_index(Children const& children, Key const& key)
{
    size_t low = 0, high = children.size();
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (children[middle].key <= key)
            low = middle + 1;
        else
            high = middle;
    }
    return low == 0 ? 0 : low - 1;
}

BTree::~BTree() = default;

ErrorOr<void> BTree::create_empty()
{
    auto root = TRY(try_make<Node>());
    root->dirty = true;
    m_root = move(root);
    m_loaded_nodes = 1;
    return {};
}

ErrorOr<void> BTree::load(BlockIndex root, u64 generation)
{
    m_root = TRY(read_node(root, generation, any_level));
    m_loaded_nodes = 1;
    return {};
}

BlockIndex BTree::root_block() const { return m_root->block; }
u64 BTree::root_generation() const { return m_root->generation; }
bool BTree::is_dirty() const { return m_root->dirty; }
size_t BTree::height() const { return m_root->level + 1u; }

ErrorOr<NonnullOwnPtr<BTree::Node>> BTree::read_node(BlockIndex block, u64 generation, u16 level)
{
    if (block < 2 || block >= m_allocator.block_count())
        return Error::from_string_literal("vultureFS: tree node out of range");
    Vector<u8> buffer;
    TRY(buffer.try_resize(block_size));
    TRY(m_store.read_blocks(block, 1, { buffer.data(), block_size }));

    NodeHeader header;
    __builtin_memcpy(&header, buffer.data(), sizeof(header));
    if (header.magic != node_magic)
        return Error::from_string_literal("vultureFS: not a tree node");
    __builtin_memset(buffer.data() + __builtin_offsetof(NodeHeader, checksum), 0, sizeof(u32));
    if (crc32c({ buffer.data(), block_size }) != header.checksum)
        return Error::from_string_literal("vultureFS: tree node checksum mismatch");
    if (header.block != block || header.generation != generation)
        return Error::from_string_literal("vultureFS: tree node is not the one expected (lost or misdirected write)");
    if (level != any_level && header.level != level)
        return Error::from_string_literal("vultureFS: tree node at the wrong level");

    auto node = TRY(try_make<Node>());
    node->level = header.level;
    node->block = block;
    node->generation = generation;
    if (node->is_leaf()) {
        size_t entries_end = sizeof(NodeHeader) + header.count * sizeof(LeafEntry);
        if (entries_end > block_size)
            return Error::from_string_literal("vultureFS: leaf entry count too large");
        TRY(node->items.try_ensure_capacity(header.count));
        for (size_t i = 0; i < header.count; ++i) {
            LeafEntry entry;
            __builtin_memcpy(&entry, buffer.data() + sizeof(NodeHeader) + i * sizeof(LeafEntry), sizeof(entry));
            if (entry.value_offset < entries_end || entry.value_offset + entry.value_size > block_size || entry.value_size > max_value_size)
                return Error::from_string_literal("vultureFS: leaf value out of bounds");
            if (i > 0 && !(node->items.last().key < entry.key))
                return Error::from_string_literal("vultureFS: leaf keys out of order");
            Item item { entry.key, {} };
            TRY(item.value.try_resize(entry.value_size));
            __builtin_memcpy(item.value.begin(), buffer.data() + entry.value_offset, entry.value_size);
            node->bytes += sizeof(LeafEntry) + entry.value_size;
            node->items.append(move(item));
        }
    } else {
        if (header.count == 0 || header.count > max_internal_entries)
            return Error::from_string_literal("vultureFS: internal node entry count out of range");
        TRY(node->children.try_ensure_capacity(header.count));
        for (size_t i = 0; i < header.count; ++i) {
            InternalEntry entry;
            __builtin_memcpy(&entry, buffer.data() + sizeof(NodeHeader) + i * sizeof(InternalEntry), sizeof(entry));
            if (i > 0 && !(node->children.last().key < entry.key))
                return Error::from_string_literal("vultureFS: internal keys out of order");
            if (entry.child_generation > generation)
                return Error::from_string_literal("vultureFS: child newer than its parent");
            node->children.append(Node::Child { entry.key, entry.child, entry.child_generation, {} });
        }
    }
    return node;
}

ErrorOr<BTree::Node*> BTree::child(Node& node, size_t index)
{
    auto& entry = node.children[index];
    if (!entry.node) {
        entry.node = TRY(read_node(entry.block, entry.generation, static_cast<u16>(node.level - 1)));
        ++m_loaded_nodes;
    }
    return entry.node.ptr();
}

ErrorOr<Optional<Vector<u8>>> BTree::find(Key const& key)
{
    Node* node = m_root.ptr();
    while (!node->is_leaf())
        node = TRY(child(*node, child_index(node->children, key)));
    size_t index = lower_bound(node->items, key);
    if (index == node->items.size() || !(node->items[index].key == key))
        return Optional<Vector<u8>> {};
    return Optional<Vector<u8>>(node->items[index].value);
}

ErrorOr<Optional<Item>> BTree::find_at_or_before(Key const& key)
{
    // Down the path to `key`; if its leaf has nothing at or before it, the
    // answer is the last item of the subtree just before.
    struct Search {
        BTree& tree;
        Key const& key;
        ErrorOr<Optional<Item>> in(Node& node)
        {
            if (node.is_leaf()) {
                size_t index = lower_bound(node.items, key);
                if (index < node.items.size() && node.items[index].key == key)
                    return Optional<Item>(node.items[index]);
                if (index == 0)
                    return Optional<Item> {};
                return Optional<Item>(node.items[index - 1]);
            }
            for (size_t index = child_index(node.children, key) + 1; index-- > 0;) {
                auto found = TRY(in(*TRY(tree.child(node, index))));
                if (found.has_value())
                    return found;
            }
            return Optional<Item> {};
        }
    };
    Search search { *this, key };
    return search.in(*m_root);
}

ErrorOr<void> BTree::insert(Key const& key, ReadonlyBytes value)
{
    VERIFY(value.size() <= max_value_size);
    auto split = TRY(insert_into(*m_root, key, value));
    if (!split.has_value())
        return {};
    // The root split: a new root above both halves.
    auto root = TRY(try_make<Node>());
    ++m_loaded_nodes;
    root->level = static_cast<u16>(m_root->level + 1);
    root->dirty = true;
    Key first = m_root->first_key();
    TRY(root->children.try_ensure_capacity(2));
    root->children.append(Node::Child { first, 0, 0, move(m_root) });
    auto right = split.release_value();
    root->children.append(Node::Child { right.key, 0, 0, move(right.node) });
    m_root = move(root);
    return {};
}

ErrorOr<Optional<BTree::Split>> BTree::insert_into(Node& node, Key const& key, ReadonlyBytes value)
{
    if (node.is_leaf()) {
        Vector<u8> bytes;
        TRY(bytes.try_resize(value.size()));
        if (!value.is_empty())
            __builtin_memcpy(bytes.begin(), value.data(), value.size());
        size_t index = lower_bound(node.items, key);
        if (index < node.items.size() && node.items[index].key == key) {
            node.bytes = node.bytes - node.items[index].value.size() + value.size();
            node.items[index].value = move(bytes);
        } else {
            TRY(node.items.try_insert(index, Item { key, move(bytes) }));
            node.bytes += sizeof(LeafEntry) + value.size();
        }
        node.dirty = true;
        if (node.bytes <= node_capacity)
            return Optional<Split> {};
        return split(node);
    }

    size_t index = child_index(node.children, key);
    auto* target = TRY(child(node, index));
    auto result = TRY(insert_into(*target, key, value));
    node.dirty = true;
    if (key < node.children[index].key)
        node.children[index].key = key;
    if (!result.has_value())
        return Optional<Split> {};
    auto right = result.release_value();
    TRY(node.children.try_insert(index + 1, Node::Child { right.key, 0, 0, move(right.node) }));
    if (node.children.size() <= max_internal_entries)
        return Optional<Split> {};
    return split(node);
}

ErrorOr<Optional<BTree::Split>> BTree::split(Node& node)
{
    auto right = TRY(try_make<Node>());
    ++m_loaded_nodes;
    right->level = node.level;
    right->dirty = true;
    if (node.is_leaf()) {
        // Half the bytes each. Values are small enough (max_value_size)
        // that both halves then fit.
        size_t prefix = 0, index = 0;
        while (index < node.items.size() && prefix < node.bytes / 2) {
            prefix += sizeof(LeafEntry) + node.items[index].value.size();
            ++index;
        }
        if (index >= node.items.size())
            index = node.items.size() - 1;
        if (index == 0)
            index = 1;
        TRY(right->items.try_ensure_capacity(node.items.size() - index));
        for (size_t i = index; i < node.items.size(); ++i) {
            right->bytes += sizeof(LeafEntry) + node.items[i].value.size();
            right->items.append(move(node.items[i]));
        }
        node.bytes -= right->bytes;
        TRY(node.items.try_resize(index));
        VERIFY(node.bytes <= node_capacity && right->bytes <= node_capacity);
    } else {
        size_t index = node.children.size() / 2;
        TRY(right->children.try_ensure_capacity(node.children.size() - index));
        for (size_t i = index; i < node.children.size(); ++i)
            right->children.append(move(node.children[i]));
        TRY(node.children.try_resize(index));
    }
    Key key = right->first_key();
    return Optional<Split>(Split { key, move(right) });
}

ErrorOr<bool> BTree::remove(Key const& key)
{
    bool removed = TRY(remove_from(*m_root, key));
    // A root with one child is replaced by that child.
    while (!m_root->is_leaf() && m_root->children.size() <= 1) {
        if (m_root->children.is_empty()) {
            m_root->level = 0;
            m_root->dirty = true;
            break;
        }
        (void)TRY(child(*m_root, 0));
        OwnPtr<Node> next = move(m_root->children[0].node);
        m_root->children.clear();
        discard(m_root);
        m_root = move(next);
        m_root->dirty = true;
    }
    return removed;
}

ErrorOr<bool> BTree::remove_from(Node& node, Key const& key)
{
    if (node.is_leaf()) {
        size_t index = lower_bound(node.items, key);
        if (index == node.items.size() || !(node.items[index].key == key))
            return false;
        node.bytes -= sizeof(LeafEntry) + node.items[index].value.size();
        node.items.remove(index);
        node.dirty = true;
        return true;
    }
    size_t index = child_index(node.children, key);
    auto* target = TRY(child(node, index));
    if (!TRY(remove_from(*target, key)))
        return false;
    node.dirty = true;
    if (target->count() == 0) {
        discard(node.children[index].node);
        node.children.remove(index);
        return true;
    }
    if (target->is_underfull())
        TRY(rebalance(node, index));
    return true;
}

// Merges an underfull child with a neighbour when both fit in one node.
ErrorOr<void> BTree::rebalance(Node& node, size_t index)
{
    if (node.children.size() < 2)
        return {};
    size_t left_index = index + 1 < node.children.size() ? index : index - 1;
    auto* left = TRY(child(node, left_index));
    auto* right = TRY(child(node, left_index + 1));
    if (left->is_leaf()) {
        if (left->bytes + right->bytes > node_capacity)
            return {};
        TRY(left->items.try_ensure_capacity(left->items.size() + right->items.size()));
        for (auto& item : right->items)
            left->items.append(move(item));
        left->bytes += right->bytes;
        right->items.clear();
    } else {
        if (left->children.size() + right->children.size() > max_internal_entries)
            return {};
        TRY(left->children.try_ensure_capacity(left->children.size() + right->children.size()));
        for (auto& entry : right->children)
            left->children.append(move(entry));
        right->children.clear();
    }
    left->dirty = true;
    discard(node.children[left_index + 1].node);
    node.children.remove(left_index + 1);
    return {};
}

// Forgets a node with nothing below it, freeing its block.
void BTree::discard(OwnPtr<Node>& node)
{
    VERIFY(node->children.is_empty());
    if (node->block != 0)
        m_allocator.free(node->block, 1);
    --m_loaded_nodes;
    node.clear();
}

ErrorOr<void> BTree::walk(Node& node, Key const& start, Visitor visit, void* context, bool& stop)
{
    if (node.is_leaf()) {
        for (size_t index = lower_bound(node.items, start); index < node.items.size(); ++index) {
            auto const& item = node.items[index];
            if (visit(context, item.key, { item.value.begin(), item.value.size() }) == Iteration::Stop) {
                stop = true;
                return {};
            }
        }
        return {};
    }
    for (size_t index = child_index(node.children, start); index < node.children.size(); ++index) {
        TRY(walk(*TRY(child(node, index)), start, visit, context, stop));
        if (stop)
            return {};
    }
    return {};
}

ErrorOr<void> BTree::write(u64 generation)
{
    if (!m_root->dirty)
        return {};
    Vector<u8> buffer;
    TRY(buffer.try_resize(block_size));
    return write_node(*m_root, generation, buffer.begin());
}

ErrorOr<void> BTree::write_node(Node& node, u64 generation, u8* buffer)
{
    if (!node.dirty)
        return {};
    for (auto& entry : node.children) {
        if (entry.node && entry.node->dirty) {
            TRY(write_node(*entry.node, generation, buffer));
            entry.block = entry.node->block;
            entry.generation = entry.node->generation;
        }
    }
    // The old copy stays intact (the allocator will not hand it out until
    // this checkpoint commits); the new one goes somewhere free.
    if (node.block != 0)
        m_allocator.free(node.block, 1);
    node.block = TRY(m_allocator.allocate());
    node.generation = generation;

    __builtin_memset(buffer, 0, block_size);
    NodeHeader header { node_magic, 0, node.block, generation, node.level, static_cast<u16>(node.count()), 0 };
    if (node.is_leaf()) {
        size_t value_end = block_size;
        for (size_t i = 0; i < node.items.size(); ++i) {
            auto const& item = node.items[i];
            value_end -= item.value.size();
            if (!item.value.is_empty())
                __builtin_memcpy(buffer + value_end, item.value.begin(), item.value.size());
            LeafEntry entry { item.key, static_cast<u16>(value_end), static_cast<u16>(item.value.size()) };
            __builtin_memcpy(buffer + sizeof(NodeHeader) + i * sizeof(LeafEntry), &entry, sizeof(entry));
        }
        VERIFY(sizeof(NodeHeader) + node.items.size() * sizeof(LeafEntry) <= value_end);
    } else {
        for (size_t i = 0; i < node.children.size(); ++i) {
            auto const& child = node.children[i];
            InternalEntry entry { child.key, child.block, child.generation };
            __builtin_memcpy(buffer + sizeof(NodeHeader) + i * sizeof(InternalEntry), &entry, sizeof(entry));
        }
    }
    __builtin_memcpy(buffer, &header, sizeof(header));
    header.checksum = crc32c({ buffer, block_size });
    __builtin_memcpy(buffer, &header, sizeof(header));
    TRY(m_store.write_blocks(node.block, 1, { buffer, block_size }));
    node.dirty = false;
    return {};
}

static size_t count_nodes(BTree::Node const&);

void BTree::trim_cache(size_t keep_nodes)
{
    if (m_loaded_nodes > keep_nodes)
        drop_children(*m_root);
}

void BTree::drop_children(Node& node)
{
    for (auto& entry : node.children) {
        if (!entry.node)
            continue;
        if (entry.node->dirty) {
            drop_children(*entry.node);
            continue;
        }
        m_loaded_nodes -= count_nodes(*entry.node);
        entry.node.clear();
    }
}

static size_t count_nodes(BTree::Node const& node)
{
    size_t count = 1;
    for (auto const& entry : node.children) {
        if (entry.node)
            count += count_nodes(*entry.node);
    }
    return count;
}

}
