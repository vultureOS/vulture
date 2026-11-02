/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>

// A reader for the Devicetree Specification's flattened format (DTB), as
// handed over by firmware. Every access is bounds-checked against the blob.

namespace DeviceTree {

struct Property {
    StringView name;
    ReadonlyBytes value;

    // The value as a null-terminated string (the first one, for string lists).
    [[nodiscard]] StringView as_string() const;
    [[nodiscard]] bool string_list_contains(StringView) const;
    [[nodiscard]] Optional<u32> as_u32() const;

    // `count` big-endian 32-bit cells starting at cell `index`, combined
    // into one number. Fails if out of range or more than two cells.
    [[nodiscard]] Optional<u64> read_cells(size_t index, size_t count) const;
    [[nodiscard]] size_t cell_count() const { return value.size() / 4; }
};

struct Node {
    StringView name;
    size_t depth { 0 };
    // How this node's "reg" is laid out: the #address-cells and
    // #size-cells of its parent.
    u32 address_cells { 2 };
    u32 size_cells { 1 };
    // Where the node's properties begin in the structure block.
    size_t properties_offset { 0 };
};

struct Region {
    u64 address { 0 };
    u64 size { 0 };
};

enum class IterationDecision {
    Continue,
    Break,
};

class FlattenedDeviceTree {
public:
    [[nodiscard]] static ErrorOr<FlattenedDeviceTree> parse(ReadonlyBytes blob);
    // A tree with nothing in it, for a machine that doesn't have one.
    [[nodiscard]] static FlattenedDeviceTree empty() { return FlattenedDeviceTree {}; }

    [[nodiscard]] size_t total_size() const { return m_blob.size(); }

    // Visits every node depth-first. `callback(Node const&)` returns an
    // IterationDecision.
    template<typename Callback>
    ErrorOr<void> for_each_node(Callback callback) const
    {
        return walk_nodes([](void* context, Node const& node) {
            return (*static_cast<Callback*>(context))(node);
        },
            &callback);
    }

    [[nodiscard]] Optional<Property> find_property(Node const&, StringView name) const;

    // "/chosen", "/pl011@9000000"; a component without "@unit" matches any unit.
    [[nodiscard]] Optional<Node> find_node_by_path(StringView path) const;

    // The first node whose "compatible" list contains `compatible`.
    [[nodiscard]] Optional<Node> find_compatible_node(StringView compatible) const;

    // The node another refers to by "phandle" (in interrupt-parent etc.).
    [[nodiscard]] Optional<Node> find_node_by_phandle(u32 phandle) const;

    // A u32 property such as "#address-cells", or `fallback` if absent.
    [[nodiscard]] u32 u32_property(Node const&, StringView name, u32 fallback) const;

    // Entry `index` of the node's "reg" property, as the processor addresses it: through the "ranges"
    // of the buses above the node.
    [[nodiscard]] Optional<Region> reg(Node const&, size_t index) const;

    // What the firmware's memory reservation block lists, one entry at a time, until there are no more.
    [[nodiscard]] Optional<Region> memory_reservation(size_t index) const;

    // The node named by /chosen's "stdout-path", resolving aliases and
    // dropping ":options".
    [[nodiscard]] Optional<Node> find_stdout_node() const;

private:
    using NodeVisitor = IterationDecision (*)(void* context, Node const&);

    FlattenedDeviceTree() = default;
    ErrorOr<void> walk_nodes(NodeVisitor, void* context) const;

    [[nodiscard]] u64 translate_through_buses(Node const&, u64 address) const;
    [[nodiscard]] Optional<u32> read_token(size_t offset) const;
    [[nodiscard]] Optional<StringView> read_string_at(ReadonlyBytes block, size_t offset) const;

    ReadonlyBytes m_blob;
    ReadonlyBytes m_structure;
    ReadonlyBytes m_strings;
};

}
