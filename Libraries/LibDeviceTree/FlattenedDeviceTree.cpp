/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Array.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace DeviceTree {

static constexpr u32 fdt_magic = 0xd00dfeed;
static constexpr u32 fdt_begin_node = 1;
static constexpr u32 fdt_end_node = 2;
static constexpr u32 fdt_prop = 3;
static constexpr u32 fdt_nop = 4;
static constexpr u32 fdt_end = 9;

static constexpr size_t header_size = 40;
static constexpr size_t max_depth = 32;

static u32 read_be32(ReadonlyBytes bytes, size_t offset)
{
    return (u32(bytes[offset]) << 24) | (u32(bytes[offset + 1]) << 16) | (u32(bytes[offset + 2]) << 8) | u32(bytes[offset + 3]);
}

static constexpr size_t align4(size_t value)
{
    return (value + 3) & ~size_t(3);
}

StringView Property::as_string() const
{
    size_t length = 0;
    while (length < value.size() && value[length] != 0)
        ++length;
    return { reinterpret_cast<char const*>(value.data()), length };
}

bool Property::string_list_contains(StringView needle) const
{
    size_t start = 0;
    while (start < value.size()) {
        size_t end = start;
        while (end < value.size() && value[end] != 0)
            ++end;
        if (StringView(reinterpret_cast<char const*>(value.data()) + start, end - start) == needle)
            return true;
        start = end + 1;
    }
    return false;
}

Optional<u32> Property::as_u32() const
{
    if (value.size() != 4)
        return {};
    return read_be32(value, 0);
}

Optional<u64> Property::read_cells(size_t index, size_t count) const
{
    if (count == 0 || count > 2 || (index + count) * 4 > value.size())
        return {};
    u64 result = 0;
    for (size_t i = 0; i < count; ++i)
        result = (result << 32) | read_be32(value, (index + i) * 4);
    return result;
}

ErrorOr<FlattenedDeviceTree> FlattenedDeviceTree::parse(ReadonlyBytes blob)
{
    if (blob.size() < header_size)
        return Error::from_string_literal("DeviceTree: blob too small");
    if (read_be32(blob, 0) != fdt_magic)
        return Error::from_string_literal("DeviceTree: bad magic");

    u32 total_size = read_be32(blob, 4);
    u32 structure_offset = read_be32(blob, 8);
    u32 strings_offset = read_be32(blob, 12);
    u32 last_compatible_version = read_be32(blob, 24);
    u32 strings_size = read_be32(blob, 32);
    u32 structure_size = read_be32(blob, 36);

    if (total_size > blob.size() || total_size < header_size)
        return Error::from_string_literal("DeviceTree: bad total size");
    if (last_compatible_version > 17)
        return Error::from_string_literal("DeviceTree: unsupported version");
    if (u64(structure_offset) + structure_size > total_size || u64(strings_offset) + strings_size > total_size || structure_offset % 4 != 0)
        return Error::from_string_literal("DeviceTree: blocks outside the blob");

    FlattenedDeviceTree tree;
    tree.m_blob = blob.slice(0, total_size);
    tree.m_structure = tree.m_blob.slice(structure_offset, structure_size);
    tree.m_strings = tree.m_blob.slice(strings_offset, strings_size);
    return tree;
}

Optional<u32> FlattenedDeviceTree::read_token(size_t offset) const
{
    if (offset + 4 > m_structure.size())
        return {};
    return read_be32(m_structure, offset);
}

Optional<StringView> FlattenedDeviceTree::read_string_at(ReadonlyBytes block, size_t offset) const
{
    if (offset >= block.size())
        return {};
    size_t length = 0;
    while (offset + length < block.size() && block[offset + length] != 0)
        ++length;
    if (offset + length == block.size())
        return {}; // Not terminated inside the block.
    return StringView(reinterpret_cast<char const*>(block.data()) + offset, length);
}

ErrorOr<void> FlattenedDeviceTree::walk_nodes(NodeVisitor visit, void* context) const
{
    // Cells declared by the node at each depth, for interpreting its children.
    struct Cells {
        u32 address;
        u32 size;
    };
    Array<Cells, max_depth + 1> cells {};
    cells[0] = { 2, 1 };

    size_t depth = 0;
    size_t offset = 0;
    for (;;) {
        auto token = read_token(offset);
        if (!token.has_value())
            return Error::from_string_literal("DeviceTree: structure block ends without FDT_END");
        offset += 4;

        switch (token.value()) {
        case fdt_begin_node: {
            auto name = read_string_at(m_structure, offset);
            if (!name.has_value())
                return Error::from_string_literal("DeviceTree: bad node name");
            offset = align4(offset + name.value().length() + 1);
            if (depth == max_depth)
                return Error::from_string_literal("DeviceTree: nodes nested too deeply");

            Node node;
            node.name = name.value();
            node.depth = depth;
            node.address_cells = cells[depth].address;
            node.size_cells = cells[depth].size;
            node.properties_offset = offset;

            ++depth;
            // Per the specification, cells default to 2 and 1 when not declared.
            cells[depth] = { 2, 1 };
            if (auto address_cells = find_property(node, "#address-cells"_sv); address_cells.has_value() && address_cells->as_u32().has_value())
                cells[depth].address = address_cells->as_u32().value();
            if (auto size_cells = find_property(node, "#size-cells"_sv); size_cells.has_value() && size_cells->as_u32().has_value())
                cells[depth].size = size_cells->as_u32().value();

            if (visit(context, node) == IterationDecision::Break)
                return {};
            break;
        }
        case fdt_end_node:
            if (depth == 0)
                return Error::from_string_literal("DeviceTree: unbalanced FDT_END_NODE");
            --depth;
            break;
        case fdt_prop: {
            auto length = read_token(offset);
            if (!length.has_value() || offset + 8 + length.value() > m_structure.size())
                return Error::from_string_literal("DeviceTree: bad property");
            offset = align4(offset + 8 + length.value());
            break;
        }
        case fdt_nop:
            break;
        case fdt_end:
            if (depth != 0)
                return Error::from_string_literal("DeviceTree: FDT_END inside a node");
            return {};
        default:
            return Error::from_string_literal("DeviceTree: unknown token");
        }
    }
}

Optional<Property> FlattenedDeviceTree::find_property(Node const& node, StringView name) const
{
    size_t offset = node.properties_offset;
    for (;;) {
        auto token = read_token(offset);
        if (!token.has_value())
            return {};
        if (token.value() == fdt_nop) {
            offset += 4;
            continue;
        }
        // Properties come before child nodes, so the first non-property ends the list.
        if (token.value() != fdt_prop)
            return {};

        auto length = read_token(offset + 4);
        auto name_offset = read_token(offset + 8);
        if (!length.has_value() || !name_offset.has_value() || offset + 12 + length.value() > m_structure.size())
            return {};
        auto property_name = read_string_at(m_strings, name_offset.value());
        if (!property_name.has_value())
            return {};
        if (property_name.value() == name)
            return Property { property_name.value(), m_structure.slice(offset + 12, length.value()) };
        offset = align4(offset + 12 + length.value());
    }
}

Optional<Node> FlattenedDeviceTree::find_node_by_path(StringView path) const
{
    if (!path.starts_with('/'))
        return {};

    // Split the path into components up front.
    Array<StringView, max_depth> components {};
    size_t component_count = 0;
    bool too_deep = false;
    path.substring_view(1).for_each_split_view('/', SplitBehavior::SkipEmpty, [&](StringView component) {
        if (component_count == components.size())
            too_deep = true;
        else
            components[component_count++] = component;
    });
    if (too_deep)
        return {};

    // How many leading components the current branch has matched so far.
    size_t matched = 0;
    Optional<Node> found;
    auto result = for_each_node([&](Node const& node) {
        if (node.depth == 0) {
            if (component_count == 0) {
                found = node;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        }
        // Coming back up out of a matched branch lowers the match to this level.
        size_t level = node.depth - 1;
        if (level < matched)
            matched = level;
        if (level != matched)
            return IterationDecision::Continue;

        auto wanted = components[matched];
        bool matches = node.name == wanted
            || (!wanted.contains('@') && node.name.starts_with(wanted) && node.name.length() > wanted.length() && node.name[wanted.length()] == '@');
        if (!matches)
            return IterationDecision::Continue;
        if (++matched == component_count) {
            found = node;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (result.is_error())
        return {};
    return found;
}

Optional<Node> FlattenedDeviceTree::find_compatible_node(StringView compatible) const
{
    Optional<Node> found;
    auto result = for_each_node([&](Node const& node) {
        auto property = find_property(node, "compatible"_sv);
        if (property.has_value() && property->string_list_contains(compatible)) {
            found = node;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (result.is_error())
        return {};
    return found;
}

Optional<Node> FlattenedDeviceTree::find_node_by_phandle(u32 phandle) const
{
    Optional<Node> found;
    auto result = for_each_node([&](Node const& node) {
        auto property = find_property(node, "phandle"_sv);
        if (property.has_value() && property->as_u32() == phandle) {
            found = node;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (result.is_error())
        return {};
    return found;
}

u32 FlattenedDeviceTree::u32_property(Node const& node, StringView name, u32 fallback) const
{
    auto property = find_property(node, name);
    if (!property.has_value())
        return fallback;
    return property->as_u32().value_or(fallback);
}

Optional<Region> FlattenedDeviceTree::reg(Node const& node, size_t index) const
{
    auto property = find_property(node, "reg"_sv);
    if (!property.has_value())
        return {};
    size_t entry_cells = node.address_cells + node.size_cells;
    auto address = property->read_cells(index * entry_cells, node.address_cells);
    if (!address.has_value())
        return {};
    Region region;
    region.address = address.value();
    if (node.size_cells > 0) {
        auto size = property->read_cells(index * entry_cells + node.address_cells, node.size_cells);
        if (!size.has_value())
            return {};
        region.size = size.value();
    }
    return region;
}

Optional<Node> FlattenedDeviceTree::find_stdout_node() const
{
    auto chosen = find_node_by_path("/chosen"_sv);
    if (!chosen.has_value())
        return {};
    auto stdout_path = find_property(chosen.value(), "stdout-path"_sv);
    if (!stdout_path.has_value())
        return {};

    auto path = stdout_path->as_string();
    if (auto options = path.find(':'); options.has_value())
        path = path.substring_view(0, options.value());

    if (!path.starts_with('/')) {
        auto aliases = find_node_by_path("/aliases"_sv);
        if (!aliases.has_value())
            return {};
        auto alias = find_property(aliases.value(), path);
        if (!alias.has_value())
            return {};
        path = alias->as_string();
    }
    return find_node_by_path(path);
}

}
