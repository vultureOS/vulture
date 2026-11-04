/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Zip.h>
#include <LibTest/TestCase.h>

namespace {

ReadonlyBytes text(char const* characters)
{
    return { reinterpret_cast<u8 const*>(characters), __builtin_strlen(characters) };
}

bool same(Vector<u8> const& bytes, ReadonlyBytes expected)
{
    return bytes.size() == expected.size() && (bytes.is_empty() || __builtin_memcmp(bytes.data(), expected.data(), expected.size()) == 0);
}

}

TEST_CASE(zip_round_trip)
{
    Archive::ZipWriter writer;
    EXPECT(!writer.add_directory("a/"_sv, 040755, 0x5a21, 0x6000).is_error());
    EXPECT(!writer.add_file("a/hello.txt"_sv, 0100644, text("hello\n"), 0x5a21, 0x6000).is_error());
    Vector<u8> repetitive;
    MUST(repetitive.try_resize(100000));
    __builtin_memset(repetitive.data(), 'x', repetitive.size());
    EXPECT(!writer.add_file("big"_sv, 0100600, repetitive.span(), 0x5a21, 0x6000).is_error());
    EXPECT(!writer.add_file("empty"_sv, 0100644, {}, 0x5a21, 0x6000).is_error());
    EXPECT(!writer.add_symbolic_link("link"_sv, "a/hello.txt"_sv, 0x5a21, 0x6000).is_error());
    auto archive = MUST(writer.finish());
    // Far smaller than what went in: it's compressed.
    EXPECT(archive.size() < 2000);

    auto reader = MUST(Archive::ZipReader::parse(archive.span()));
    auto const& entries = reader.entries();
    EXPECT_EQ(entries.size(), 5u);
    // (With no slash: that it's a directory is what is_directory is for.)
    EXPECT(entries[0].path.view() == "a"_sv);
    EXPECT(entries[0].is_directory);
    EXPECT(entries[1].path.view() == "a/hello.txt"_sv);
    EXPECT(!entries[1].is_directory);
    EXPECT_EQ(entries[1].mode & 0777u, 0644u);
    EXPECT_EQ(entries[1].dos_date, 0x5a21);
    EXPECT(same(MUST(reader.read(entries[1])), text("hello\n")));
    EXPECT_EQ(entries[2].size, 100000u);
    EXPECT(entries[2].compressed_size < 2000);
    EXPECT(same(MUST(reader.read(entries[2])), repetitive.span()));
    EXPECT_EQ(entries[3].size, 0u);
    EXPECT(MUST(reader.read(entries[3])).is_empty());
    EXPECT(entries[4].is_symbolic_link);
    EXPECT(same(MUST(reader.read(entries[4])), text("a/hello.txt")));
}

TEST_CASE(what_doesnt_compress_is_stored)
{
    // Something with nothing to find in it.
    Vector<u8> noise;
    MUST(noise.try_resize(2048));
    u32 state = 0x12345678;
    for (auto& byte : noise) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<u8>(state);
    }
    Archive::ZipWriter writer;
    EXPECT(!writer.add_file("noise"_sv, 0100644, noise.span(), 0, 0).is_error());
    auto archive = MUST(writer.finish());
    auto reader = MUST(Archive::ZipReader::parse(archive.span()));
    EXPECT_EQ(reader.entries()[0].method, 0);
    EXPECT(same(MUST(reader.read(reader.entries()[0])), noise.span()));
}

TEST_CASE(what_isnt_right_is_refused)
{
    EXPECT(Archive::ZipReader::parse(text("this isn't a zip file at all, not even slightly")).is_error());
    EXPECT(Archive::ZipReader::parse({}).is_error());

    Archive::ZipWriter writer;
    EXPECT(!writer.add_file("file"_sv, 0100644, text("the contents of the file"), 0, 0).is_error());
    auto archive = MUST(writer.finish());

    // Anything that's been changed doesn't get through: the checksum says so.
    {
        Vector<u8> damaged;
        MUST(damaged.try_resize(archive.size()));
        __builtin_memcpy(damaged.data(), archive.data(), archive.size());
        // Its first byte of data: past a local header of 30 bytes, and the name.
        damaged[30 + 4] ^= 0xff;
        auto reader = MUST(Archive::ZipReader::parse(damaged.span()));
        EXPECT(reader.read(reader.entries()[0]).is_error());
    }

    // Each length of it, cut short: an error, and no more than that.
    for (size_t length = 0; length < archive.size(); ++length) {
        auto reader = Archive::ZipReader::parse(archive.span().slice(0, length));
        if (!reader.is_error() && !reader.value().entries().is_empty())
            (void)reader.value().read(reader.value().entries()[0]);
    }
}
