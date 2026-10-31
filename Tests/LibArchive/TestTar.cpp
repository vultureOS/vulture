/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Tar.h>
#include <LibTest/TestCase.h>

namespace {

class BufferSink final : public Archive::TarSink {
public:
    void write(ReadonlyBytes bytes) override
    {
        for (u8 byte : bytes) {
            if (m_size < sizeof(m_buffer))
                m_buffer[m_size++] = byte;
        }
    }

    [[nodiscard]] ReadonlyBytes bytes() const { return { m_buffer, m_size }; }
    [[nodiscard]] Bytes mutable_bytes() { return { m_buffer, m_size }; }

private:
    u8 m_buffer[64 * 1024];
    size_t m_size { 0 };
};

BufferSink s_sink;

ReadonlyBytes text(char const* characters)
{
    return { reinterpret_cast<u8 const*>(characters), __builtin_strlen(characters) };
}

}

TEST_CASE(tar_round_trip)
{
    s_sink = {};
    Archive::TarWriter writer(s_sink);
    EXPECT(!writer.add_directory("sbin"_sv, 0755).is_error());
    EXPECT(!writer.add_file("sbin/init"_sv, 0755, text("\x7f"
                                                       "ELF fake binary"))
            .is_error());
    EXPECT(!writer.add_file("etc/motd"_sv, 0644, text("Welcome\n")).is_error());
    EXPECT(!writer.add_file("empty"_sv, 0644, {}).is_error());
    writer.finish();
    EXPECT_EQ(s_sink.bytes().size() % 512, 0u);

    auto reader = MUST(Archive::TarReader::parse(s_sink.bytes()));
    auto init = reader.find("/sbin/init"_sv);
    EXPECT(init.has_value());
    EXPECT(init->type == Archive::TarEntryType::File);
    EXPECT_EQ(init->mode, 0755u);
    EXPECT_EQ(init->data.size(), 16u);

    auto motd = reader.find("./etc/motd"_sv);
    EXPECT(motd.has_value());
    EXPECT(StringView(reinterpret_cast<char const*>(motd->data.data()), motd->data.size()) == "Welcome\n"_sv);

    auto directory = reader.find("sbin/"_sv);
    EXPECT(directory.has_value());
    EXPECT(directory->type == Archive::TarEntryType::Directory);

    EXPECT(reader.find("empty"_sv).has_value());
    EXPECT_EQ(reader.find("empty"_sv)->data.size(), 0u);
    EXPECT(!reader.find("missing"_sv).has_value());

    size_t count = 0;
    MUST(reader.for_each_entry([&](Archive::TarEntry const&) {
        ++count;
        return Archive::IterationDecision::Continue;
    }));
    EXPECT_EQ(count, 4u);
}

TEST_CASE(tar_long_paths_use_the_prefix)
{
    s_sink = {};
    Archive::TarWriter writer(s_sink);
    char const* long_path = "System/Library/Frameworks/SomeVeryLongFrameworkName.framework/Versions/A/Resources/en.lproj/Localizable.strings.extra";
    EXPECT(!writer.add_file(StringView(long_path), 0644, text("x")).is_error());
    writer.finish();
    auto reader = MUST(Archive::TarReader::parse(s_sink.bytes()));
    EXPECT(reader.find(StringView(long_path)).has_value());
}

TEST_CASE(tar_rejects_corruption)
{
    s_sink = {};
    Archive::TarWriter writer(s_sink);
    EXPECT(!writer.add_file("file"_sv, 0644, text("data")).is_error());
    writer.finish();
    // Flip a byte of the name: the checksum no longer matches.
    s_sink.mutable_bytes()[0] ^= 1;
    EXPECT(Archive::TarReader::parse(s_sink.bytes()).is_error());

    u8 odd_size[100] {};
    EXPECT(Archive::TarReader::parse({ odd_size, sizeof(odd_size) }).is_error());
}
