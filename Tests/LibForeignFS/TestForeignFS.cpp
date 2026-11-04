/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/Format.h>
#include <LibForeignFS/Volume.h>
#include <LibTest/TestCase.h>

using namespace ForeignFS;

namespace {

class MemoryStore final : public Store {
public:
    explicit MemoryStore(u64 sectors)
        : m_sectors(sectors)
    {
        MUST(m_bytes.try_resize(sectors * sector_size));
        __builtin_memset(m_bytes.begin(), 0, m_bytes.size());
    }

    [[nodiscard]] u64 sector_count() const override { return m_sectors; }
    [[nodiscard]] bool is_read_only() const override { return false; }
    ErrorOr<void> read_sectors(u64 first, size_t count, Bytes buffer) override
    {
        __builtin_memcpy(buffer.data(), m_bytes.begin() + first * sector_size, count * sector_size);
        return {};
    }
    ErrorOr<void> write_sectors(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        __builtin_memcpy(m_bytes.begin() + first * sector_size, buffer.data(), count * sector_size);
        return {};
    }
    ErrorOr<void> flush() override { return {}; }

private:
    u64 m_sectors;
    Vector<u8> m_bytes;
};

extern "C" long write(int fd, void const* buffer, unsigned long count);

class StandardProblem final : public FormatSink {
public:
    void write(StringView text) override { (void)::write(2, text.characters_without_null_termination(), text.length()); }
};

i64 fixed_clock()
{
    return 1793785502;
}

struct VolumeKind {
    StringView name;
    u64 mebibytes;
    unsigned bits;
};

constexpr VolumeKind s_kinds[] = {
    { "FAT12"_sv, 2, 12 },
    { "FAT16"_sv, 64, 16 },
    { "FAT32"_sv, 64, 32 },
    { "exFAT"_sv, 64, 0 },
};

NonnullOwnPtr<Volume> format_and_open(MemoryStore& store, VolumeKind const& kind)
{
    if (kind.bits)
        MUST(format_fat(store, "TESTING"_sv, fixed_clock, kind.bits));
    else
        MUST(format_exfat(store, "TESTING"_sv, fixed_clock));
    return MUST(open_volume(store, fixed_clock, false));
}

void expect_clean(MemoryStore& store, VolumeKind const& kind)
{
    auto report = kind.bits ? check_fat(store, fixed_clock) : check_exfat(store, fixed_clock);
    EXPECT(!report.is_error());
    if (report.is_error())
        return;
    for (auto const& problem : report.value().problems) {
        StandardProblem output;
        format_to(output, "    {}: {}\n", kind.name, problem.view());
        EXPECT(false);
    }
}

size_t count_entries(Volume& volume, NodeIndex directory)
{
    size_t count = 0;
    MUST(volume.for_each_entry(directory, [&](StringView, NodeInfo const&) {
        ++count;
        return Iteration::Continue;
    }));
    return count;
}

}

TEST_CASE(each_is_recognised_and_opens_empty)
{
    for (auto const& kind : s_kinds) {
        MemoryStore store(kind.mebibytes * 1024 * 1024 / Store::sector_size);
        auto volume = format_and_open(store, kind);
        EXPECT_EQ(volume->label(), "TESTING"_sv);
        EXPECT_EQ(count_entries(*volume, volume->root()), 0u);
        EXPECT(recognise(store) == (kind.bits ? ForeignFS::Kind::FAT : ForeignFS::Kind::ExFAT));
        auto const statistics = MUST(volume->statistics());
        EXPECT(statistics.free_blocks > 0 && statistics.free_blocks <= statistics.block_count);
        expect_clean(store, kind);
    }
}

TEST_CASE(files_directories_and_names_come_back)
{
    for (auto const& kind : s_kinds) {
        MemoryStore store(kind.mebibytes * 1024 * 1024 / Store::sector_size);
        {
            auto volume = format_and_open(store, kind);
            auto const root = volume->root();
            auto const file = MUST(volume->create(root, "A file with a long name.text"_sv, false));
            EXPECT_EQ(MUST(volume->write(file.index, 0, { reinterpret_cast<u8 const*>("hello"), 5 })), 5u);
            auto const directory = MUST(volume->create(root, "Folder"_sv, true));
            EXPECT(directory.is_directory);
            auto const inner = MUST(volume->create(directory.index, "café 😀.txt"_sv, false));
            MUST(volume->write(inner.index, 0, { reinterpret_cast<u8 const*>("inside"), 6 }));
            EXPECT_EQ(volume->create(root, "Folder"_sv, false).error().code(), EEXIST);
            EXPECT_EQ(volume->create(root, "FOLDER"_sv, false).error().code(), EEXIST);
            EXPECT_EQ(volume->create(root, "bad/name"_sv, false).error().code(), EINVAL);
            MUST(volume->flush());
        }
        expect_clean(store, kind);
        auto volume = MUST(open_volume(store, fixed_clock, false));
        auto const root = volume->root();
        EXPECT_EQ(count_entries(*volume, root), 2u);
        auto const file = MUST(volume->lookup(root, "a FILE with a long NAME.TEXT"_sv));
        EXPECT_EQ(file.size, 5u);
        u8 bytes[16];
        EXPECT_EQ(MUST(volume->read(file.index, 0, { bytes, sizeof(bytes) })), 5u);
        EXPECT(__builtin_memcmp(bytes, "hello", 5) == 0);
        auto const directory = MUST(volume->lookup(root, "folder"_sv));
        EXPECT(directory.is_directory);
        auto const inner = MUST(volume->lookup(directory.index, "CAFÉ 😀.TXT"_sv));
        EXPECT_EQ(MUST(volume->read(inner.index, 0, { bytes, sizeof(bytes) })), 6u);
        EXPECT(__builtin_memcmp(bytes, "inside", 6) == 0);
        EXPECT_EQ(file.modified, fixed_clock() & ~1ll);
    }
}

TEST_CASE(large_files_and_truncation)
{
    for (auto const& kind : s_kinds) {
        MemoryStore store(kind.mebibytes * 1024 * 1024 / Store::sector_size);
        auto volume = format_and_open(store, kind);
        auto const root = volume->root();
        auto const file = MUST(volume->create(root, "large"_sv, false));
        size_t const size = kind.bits == 12 ? 300 * 1024 : 5 * 1024 * 1024 + 321;
        Vector<u8> bytes;
        MUST(bytes.try_resize(size));
        for (size_t i = 0; i < size; ++i)
            bytes[i] = static_cast<u8>(i ^ (i >> 9));
        EXPECT_EQ(MUST(volume->write(file.index, 0, bytes.span())), size);
        Vector<u8> back;
        MUST(back.try_resize(size));
        EXPECT_EQ(MUST(volume->read(file.index, 0, { back.begin(), size })), size);
        EXPECT(__builtin_memcmp(back.begin(), bytes.begin(), size) == 0);

        // Past the end reads as zeros, and what's freed is free.
        auto const before = MUST(volume->statistics()).free_blocks;
        MUST(volume->truncate(file.index, 100));
        EXPECT(MUST(volume->statistics()).free_blocks > before);
        EXPECT_EQ(MUST(volume->info(file.index)).size, 100u);
        MUST(volume->write(file.index, 8000, { reinterpret_cast<u8 const*>("x"), 1 }));
        u8 middle[4];
        EXPECT_EQ(MUST(volume->read(file.index, 200, { middle, 4 })), 4u);
        EXPECT(middle[0] == 0 && middle[3] == 0);
        MUST(volume->truncate(file.index, 20000));
        EXPECT_EQ(MUST(volume->read(file.index, 19000, { middle, 4 })), 4u);
        EXPECT(middle[0] == 0 && middle[3] == 0);
        MUST(volume->flush());
        expect_clean(store, kind);
    }
}

TEST_CASE(removing_and_renaming)
{
    for (auto const& kind : s_kinds) {
        MemoryStore store(kind.mebibytes * 1024 * 1024 / Store::sector_size);
        auto volume = format_and_open(store, kind);
        auto const root = volume->root();
        auto const directory = MUST(volume->create(root, "folder"_sv, true));
        auto const inner = MUST(volume->create(directory.index, "inner"_sv, true));
        auto const file = MUST(volume->create(inner.index, "file.txt"_sv, false));
        MUST(volume->write(file.index, 0, { reinterpret_cast<u8 const*>("text"), 4 }));
        EXPECT_EQ(volume->remove(root, "folder"_sv).error().code(), ENOTEMPTY);

        // Its name, spelled another way.
        MUST(volume->rename(inner.index, "file.txt"_sv, inner.index, "FILE.txt"_sv));
        bool found = false;
        MUST(volume->for_each_entry(inner.index, [&](StringView name, NodeInfo const&) {
            found = found || name == "FILE.txt"_sv;
            return Iteration::Continue;
        }));
        EXPECT(found);
        // To another folder, the same file, which keeps working.
        MUST(volume->rename(inner.index, "FILE.txt"_sv, root, "moved with a long name.txt"_sv));
        EXPECT_EQ(MUST(volume->info(file.index)).size, 4u);
        MUST(volume->write(file.index, 4, { reinterpret_cast<u8 const*>("more"), 4 }));
        u8 bytes[8];
        auto const moved = MUST(volume->lookup(root, "moved with a long name.txt"_sv));
        EXPECT_EQ(moved.index, file.index);
        EXPECT_EQ(MUST(volume->read(moved.index, 0, { bytes, 8 })), 8u);
        EXPECT(__builtin_memcmp(bytes, "textmore", 8) == 0);
        // A directory moved, whose ".." says where it is.
        MUST(volume->rename(directory.index, "inner"_sv, root, "up here"_sv));
        auto const up = MUST(volume->lookup(root, "up here"_sv));
        EXPECT_EQ(MUST(volume->lookup(up.index, ".."_sv)).index, root);
        EXPECT_EQ(volume->rename(root, "up here"_sv, up.index, "in itself"_sv).error().code(), EINVAL);
        // Onto what's there, which goes.
        auto const other = MUST(volume->create(root, "other"_sv, false));
        MUST(volume->rename(root, "other"_sv, root, "moved with a long name.txt"_sv));
        EXPECT_EQ(MUST(volume->lookup(root, "moved with a long name.txt"_sv)).index, other.index);
        volume->forget(file.index);

        auto const free_before = MUST(volume->statistics()).free_blocks;
        auto const file_going = MUST(volume->create(root, "going"_sv, false));
        Vector<u8> bytes_going;
        MUST(bytes_going.try_resize(256 * 1024));
        __builtin_memset(bytes_going.begin(), 1, bytes_going.size());
        MUST(volume->write(file_going.index, 0, bytes_going.span()));
        MUST(volume->remove(root, "going"_sv));
        EXPECT_EQ(volume->lookup(root, "going"_sv).error().code(), ENOENT);
        // While it's in use it still reads, and takes up room.
        EXPECT_EQ(MUST(volume->read(file_going.index, 0, { bytes, 8 })), 8u);
        volume->forget(file_going.index);
        EXPECT_EQ(MUST(volume->statistics()).free_blocks, free_before);

        MUST(volume->remove(root, "up here"_sv));
        volume->forget(up.index);
        EXPECT_EQ(volume->remove(directory.index, "nothing"_sv).error().code(), ENOENT);
        MUST(volume->flush());
        expect_clean(store, kind);
    }
}

TEST_CASE(a_directory_can_hold_a_lot)
{
    for (auto const& kind : s_kinds) {
        if (kind.bits == 12)
            continue;
        MemoryStore store(kind.mebibytes * 1024 * 1024 / Store::sector_size);
        auto volume = format_and_open(store, kind);
        auto const directory = MUST(volume->create(volume->root(), "many"_sv, true));
        for (int i = 0; i < 600; ++i) {
            StringBuilder name;
            name.appendff("A file with a long name, number {}.txt", i);
            MUST(volume->create(directory.index, MUST(name.to_string()).view(), false));
        }
        EXPECT_EQ(count_entries(*volume, directory.index), 600u);
        for (int i = 0; i < 600; i += 2) {
            StringBuilder name;
            name.appendff("A file with a long name, number {}.txt", i);
            MUST(volume->remove(directory.index, MUST(name.to_string()).view()));
        }
        EXPECT_EQ(count_entries(*volume, directory.index), 300u);
        auto const made = MUST(volume->lookup(directory.index, "a file with a long name, number 1.TXT"_sv));
        EXPECT(!made.is_directory);
        MUST(volume->flush());
        expect_clean(store, kind);
    }
}

TEST_CASE(a_full_volume_says_so)
{
    for (auto const& kind : s_kinds) {
        u64 const mebibytes = kind.bits == 12 ? 4 : 8;
        MemoryStore store(mebibytes * 1024 * 1024 / Store::sector_size);
        VolumeKind const small { kind.name, mebibytes, kind.bits == 32 ? 16 : kind.bits };
        auto volume = format_and_open(store, small);
        auto const file = MUST(volume->create(volume->root(), "fill"_sv, false));
        Vector<u8> chunk;
        MUST(chunk.try_resize(64 * 1024));
        __builtin_memset(chunk.begin(), 7, chunk.size());
        u64 written = 0;
        for (;;) {
            auto result = volume->write(file.index, written, chunk.span());
            if (result.is_error()) {
                EXPECT_EQ(result.error().code(), ENOSPC);
                break;
            }
            written += result.value();
            EXPECT(written <= 8 * 1024 * 1024);
        }
        EXPECT(written > 1024 * 1024);
        EXPECT_EQ(MUST(volume->statistics()).free_blocks, 0u);
        MUST(volume->truncate(file.index, 0));
        MUST(volume->flush());
        expect_clean(store, small);
    }
}

TEST_CASE(bad_names_and_too_long)
{
    for (auto const& kind : s_kinds) {
        MemoryStore store(kind.mebibytes * 1024 * 1024 / Store::sector_size);
        auto volume = format_and_open(store, kind);
        auto const root = volume->root();
        StringBuilder long_name;
        for (int i = 0; i < 255; ++i)
            long_name.append('a');
        MUST(volume->create(root, MUST(long_name.to_string()).view(), false));
        long_name.append('b');
        EXPECT_EQ(volume->create(root, MUST(long_name.to_string()).view(), false).error().code(), ENAMETOOLONG);
        EXPECT_EQ(volume->lookup(root, "no such thing"_sv).error().code(), ENOENT);
        EXPECT_EQ(volume->read(root, 0, {}).error().code(), EISDIR);
        MUST(volume->flush());
        expect_clean(store, kind);
    }
}
