/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibDeviceTree/FlattenedDeviceTree.h>
#include <LibTest/TestCase.h>

namespace {

// Writes a DTB the way firmware would, so the reader can be tested on
// trees with known contents.
class Builder {
public:
    void begin_node(StringView name)
    {
        token(1);
        for (char character : name)
            m_structure[m_structure_size++] = static_cast<u8>(character);
        m_structure[m_structure_size++] = 0;
        pad();
    }

    void end_node() { token(2); }

    void property(StringView name, ReadonlyBytes value)
    {
        token(3);
        put_be32(m_structure, m_structure_size, static_cast<u32>(value.size()));
        put_be32(m_structure, m_structure_size + 4, add_string(name));
        m_structure_size += 8;
        for (u8 byte : value)
            m_structure[m_structure_size++] = byte;
        pad();
    }

    void property_string(StringView name, StringView value)
    {
        u8 bytes[128] {};
        value.bytes().copy_to(bytes);
        property(name, ReadonlyBytes(bytes, value.length() + 1));
    }

    template<typename... Cells>
    void property_cells(StringView name, Cells... cells)
    {
        u32 const values[] { static_cast<u32>(cells)... };
        u8 bytes[sizeof(values)];
        for (size_t i = 0; i < sizeof...(Cells); ++i)
            put_be32(bytes, i * 4, values[i]);
        property(name, ReadonlyBytes(bytes, sizeof(bytes)));
    }

    ReadonlyBytes finish()
    {
        token(9);
        size_t structure_offset = 40 + 16; // header + empty memory reservation map
        size_t strings_offset = structure_offset + m_structure_size;
        size_t total = strings_offset + m_strings_size;
        put_be32(m_blob, 0, 0xd00dfeed);
        put_be32(m_blob, 4, static_cast<u32>(total));
        put_be32(m_blob, 8, static_cast<u32>(structure_offset));
        put_be32(m_blob, 12, static_cast<u32>(strings_offset));
        put_be32(m_blob, 16, 40);
        put_be32(m_blob, 20, 17);
        put_be32(m_blob, 24, 16);
        put_be32(m_blob, 32, static_cast<u32>(m_strings_size));
        put_be32(m_blob, 36, static_cast<u32>(m_structure_size));
        __builtin_memcpy(m_blob + structure_offset, m_structure, m_structure_size);
        __builtin_memcpy(m_blob + strings_offset, m_strings, m_strings_size);
        return { m_blob, total };
    }

private:
    static void put_be32(u8* bytes, size_t offset, u32 value)
    {
        bytes[offset] = static_cast<u8>(value >> 24);
        bytes[offset + 1] = static_cast<u8>(value >> 16);
        bytes[offset + 2] = static_cast<u8>(value >> 8);
        bytes[offset + 3] = static_cast<u8>(value);
    }

    void token(u32 value)
    {
        put_be32(m_structure, m_structure_size, value);
        m_structure_size += 4;
    }

    void pad()
    {
        while (m_structure_size % 4 != 0)
            m_structure[m_structure_size++] = 0;
    }

    u32 add_string(StringView name)
    {
        u32 offset = static_cast<u32>(m_strings_size);
        for (char character : name)
            m_strings[m_strings_size++] = static_cast<u8>(character);
        m_strings[m_strings_size++] = 0;
        return offset;
    }

    u8 m_structure[2048] {};
    size_t m_structure_size { 0 };
    u8 m_strings[512] {};
    size_t m_strings_size { 0 };
    alignas(8) u8 m_blob[4096] {};
};

// Shaped like the tree QEMU's "virt" machine hands over.
ReadonlyBytes build_virt_like_tree(Builder& builder)
{
    builder.begin_node(""_sv);
    builder.property_cells("#address-cells"_sv, 2);
    builder.property_cells("#size-cells"_sv, 2);
    builder.property_string("compatible"_sv, "linux,dummy-virt"_sv);
    builder.property_string("model"_sv, "Test Machine"_sv);

    builder.begin_node("chosen"_sv);
    builder.property_string("stdout-path"_sv, "serial0:115200n8"_sv);
    builder.end_node();

    builder.begin_node("aliases"_sv);
    builder.property_string("serial0"_sv, "/pl011@9000000"_sv);
    builder.end_node();

    builder.begin_node("pl011@9000000"_sv);
    u8 const compatible[] = "arm,pl011\0arm,primecell";
    builder.property("compatible"_sv, ReadonlyBytes(compatible, sizeof(compatible)));
    builder.property_cells("reg"_sv, 0, 0x09000000, 0, 0x1000);
    builder.end_node();

    builder.begin_node("soc"_sv);
    builder.property_cells("#address-cells"_sv, 1);
    builder.property_cells("#size-cells"_sv, 1);
    builder.begin_node("uart@1000"_sv);
    builder.property_string("compatible"_sv, "ns16550a"_sv);
    builder.property_cells("reg"_sv, 0x1000, 0x100, 0x2000, 0x100);
    builder.end_node();
    builder.end_node();

    builder.end_node();
    return builder.finish();
}

}

TEST_CASE(finds_nodes_by_path)
{
    Builder builder;
    auto tree = MUST(DeviceTree::FlattenedDeviceTree::parse(build_virt_like_tree(builder)));

    auto root = tree.find_node_by_path("/"_sv);
    EXPECT(root.has_value());
    EXPECT_EQ(tree.find_property(root.value(), "model"_sv)->as_string(), "Test Machine"_sv);

    auto chosen = tree.find_node_by_path("/chosen"_sv);
    EXPECT(chosen.has_value());
    EXPECT_EQ(chosen->name, "chosen"_sv);

    auto uart = tree.find_node_by_path("/soc/uart"_sv);
    EXPECT(uart.has_value());
    EXPECT_EQ(uart->name, "uart@1000"_sv);
    EXPECT_EQ(tree.find_node_by_path("/soc/uart@1000"_sv)->name, "uart@1000"_sv);

    EXPECT(!tree.find_node_by_path("/soc/missing"_sv).has_value());
    EXPECT(!tree.find_node_by_path("/uart@1000"_sv).has_value());
    EXPECT(!tree.find_node_by_path("relative"_sv).has_value());
}

TEST_CASE(reads_reg_with_parent_cells)
{
    Builder builder;
    auto tree = MUST(DeviceTree::FlattenedDeviceTree::parse(build_virt_like_tree(builder)));

    auto pl011 = tree.find_compatible_node("arm,primecell"_sv);
    EXPECT(pl011.has_value());
    EXPECT_EQ(pl011->name, "pl011@9000000"_sv);
    auto region = tree.reg(pl011.value(), 0);
    EXPECT(region.has_value());
    EXPECT_EQ(region->address, 0x09000000u);
    EXPECT_EQ(region->size, 0x1000u);
    EXPECT(!tree.reg(pl011.value(), 1).has_value());

    auto uart = tree.find_compatible_node("ns16550a"_sv);
    EXPECT_EQ(uart->address_cells, 1u);
    EXPECT_EQ(tree.reg(uart.value(), 1)->address, 0x2000u);
}

TEST_CASE(resolves_stdout_through_aliases)
{
    Builder builder;
    auto tree = MUST(DeviceTree::FlattenedDeviceTree::parse(build_virt_like_tree(builder)));

    auto serial = tree.find_stdout_node();
    EXPECT(serial.has_value());
    EXPECT_EQ(serial->name, "pl011@9000000"_sv);
    EXPECT(tree.find_property(serial.value(), "compatible"_sv)->string_list_contains("arm,pl011"_sv));
    EXPECT(!tree.find_property(serial.value(), "compatible"_sv)->string_list_contains("arm"_sv));
}

TEST_CASE(rejects_malformed_blobs)
{
    u8 tiny[8] {};
    EXPECT(DeviceTree::FlattenedDeviceTree::parse(ReadonlyBytes(tiny, sizeof(tiny))).is_error());

    Builder builder;
    auto blob = build_virt_like_tree(builder);
    alignas(8) u8 corrupted[4096];
    blob.copy_to(Bytes(corrupted, sizeof(corrupted)));
    corrupted[0] = 0;
    EXPECT(DeviceTree::FlattenedDeviceTree::parse(ReadonlyBytes(corrupted, blob.size())).is_error());

    // An unterminated structure block parses, but walking it fails cleanly.
    Builder unterminated;
    unterminated.begin_node(""_sv);
    auto partial = unterminated.finish();
    auto tree = MUST(DeviceTree::FlattenedDeviceTree::parse(partial));
    EXPECT(tree.for_each_node([](auto const&) { return DeviceTree::IterationDecision::Continue; }).is_error());
}
