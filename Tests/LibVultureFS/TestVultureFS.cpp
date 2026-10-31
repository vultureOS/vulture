/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/FastRandom.h>
#include <LibTest/TestCase.h>
#include <LibVultureFS/Check.h>
#include <LibVultureFS/Volume.h>

using namespace VultureFS;

namespace {

// A disk in memory with a volatile write cache: writes since the last
// flush may be lost, reordered or torn when the power goes. It can also
// be told to lose power after a number of writes.
class MemoryStore final : public BlockStore {
public:
    explicit MemoryStore(u64 blocks)
        : m_blocks(blocks)
    {
        MUST(m_current.try_resize(blocks * block_size));
        MUST(m_durable.try_resize(blocks * block_size));
        for (size_t i = 0; i < m_current.size(); ++i)
            m_current[i] = m_durable[i] = 0;
    }

    [[nodiscard]] u64 block_count() const override { return m_blocks; }

    ErrorOr<void> read_blocks(BlockIndex first, size_t count, Bytes buffer) override
    {
        VERIFY(first + count <= m_blocks && buffer.size() >= count * block_size);
        if (m_dead)
            return Error::from_errno(EIO);
        __builtin_memcpy(buffer.data(), m_current.begin() + first * block_size, count * block_size);
        ++m_reads;
        return {};
    }

    ErrorOr<void> write_blocks(BlockIndex first, size_t count, ReadonlyBytes buffer) override
    {
        VERIFY(first + count <= m_blocks && buffer.size() >= count * block_size);
        if (consume_power())
            return Error::from_errno(EIO);
        for (size_t i = 0; i < count; ++i) {
            Pending pending { first + i, {} };
            MUST(pending.data.try_resize(block_size));
            __builtin_memcpy(pending.data.begin(), buffer.data() + i * block_size, block_size);
            __builtin_memcpy(m_current.begin() + (first + i) * block_size, pending.data.begin(), block_size);
            m_pending.append(move(pending));
        }
        ++m_writes;
        return {};
    }

    ErrorOr<void> flush() override
    {
        if (consume_power())
            return Error::from_errno(EIO);
        for (auto const& pending : m_pending)
            __builtin_memcpy(m_durable.begin() + pending.block * block_size, pending.data.begin(), block_size);
        m_pending.clear();
        return {};
    }

    // Loses power after `operations` more writes or flushes.
    void fail_after(u64 operations) { m_power_left = static_cast<i64>(operations); }
    void never_fail() { m_power_left = -1; }

    // Power cut: some of the unflushed writes reach the disk, in order, the
    // last of them maybe only partly; the rest are lost.
    void crash(FastRandom& random)
    {
        for (auto const& pending : m_pending) {
            if (random.next_below(2) == 0)
                continue;
            size_t length = block_size;
            if (random.next_below(4) == 0)
                length = random.next_below(8) * 512;
            __builtin_memcpy(m_durable.begin() + pending.block * block_size, pending.data.begin(), length);
        }
        m_pending.clear();
        __builtin_memcpy(m_current.begin(), m_durable.begin(), m_current.size());
        m_dead = false;
        m_power_left = -1;
    }

    // Power cut where nothing unflushed survives.
    void crash_losing_everything()
    {
        m_pending.clear();
        __builtin_memcpy(m_current.begin(), m_durable.begin(), m_current.size());
        m_dead = false;
        m_power_left = -1;
    }

    [[nodiscard]] bool is_dead() const { return m_dead; }

    // Damages a byte on the disk, as a failing sector might.
    void corrupt(BlockIndex block, size_t offset)
    {
        m_durable[block * block_size + offset] ^= 0x40;
        m_current[block * block_size + offset] ^= 0x40;
    }

private:
    struct Pending {
        BlockIndex block;
        Vector<u8> data;
    };

    bool consume_power()
    {
        if (m_dead)
            return true;
        if (m_power_left == 0) {
            m_dead = true;
            return true;
        }
        if (m_power_left > 0)
            --m_power_left;
        return false;
    }

    u64 m_blocks;
    Vector<u8> m_current;
    Vector<u8> m_durable;
    Vector<Pending> m_pending;
    i64 m_power_left { -1 };
    bool m_dead { false };
    u64 m_reads { 0 };
    u64 m_writes { 0 };
};

template<size_t N, typename... Args>
size_t format_to_buffer(char (&buffer)[N], CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    FixedStringBuilder<N - 1> builder;
    builder.append_formatted(format, args...);
    auto view = builder.view();
    __builtin_memcpy(buffer, view.characters_without_null_termination(), view.length());
    return view.length();
}

i64 s_time = 1'700'000'000'000'000'000;
i64 test_clock() { return s_time += 1000; }

u8 const s_uuid[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

void expect_clean(BlockStore& store)
{
    auto report = MUST(check_volume(store));
    for (auto const& problem : report.problems) {
        Test::print(problem.view());
        Test::print("\n"_sv);
    }
    EXPECT(report.is_clean());
}

ReadonlyBytes text(char const* characters)
{
    return { reinterpret_cast<u8 const*>(characters), __builtin_strlen(characters) };
}

Vector<u8> read_all(Volume& volume, InodeNumber inode)
{
    auto record = MUST(volume.inode(inode));
    Vector<u8> data;
    MUST(data.try_resize(record.size));
    EXPECT_EQ(MUST(volume.read(inode, 0, { data.begin(), data.size() })), data.size());
    return data;
}

bool equals(Vector<u8> const& data, ReadonlyBytes expected)
{
    return data.size() == expected.size() && (expected.is_empty() || __builtin_memcmp(data.begin(), expected.data(), expected.size()) == 0);
}

}

TEST_CASE(format_mount_and_check_an_empty_volume)
{
    MemoryStore store(1024);
    {
        auto volume = MUST(Volume::format(store, "Macintosh HD"_sv, s_uuid, test_clock));
        EXPECT(volume->name() == "Macintosh HD"_sv);
        EXPECT_EQ(volume->statistics().generation, 1u);
    }
    expect_clean(store);
    auto volume = MUST(Volume::mount(store, test_clock));
    auto root = MUST(volume->inode(root_directory));
    EXPECT_EQ(root.mode, mode_directory | 0755);
    EXPECT_EQ(root.link_count, 2u);
    EXPECT_EQ(MUST(volume->lookup(root_directory, ".."_sv)).inode, root_directory);
    EXPECT(volume->lookup(root_directory, "missing"_sv).error().code() == ENOENT);
    auto report = MUST(check_volume(store));
    EXPECT_EQ(report.directories, 1u);
    EXPECT_EQ(report.generation, 1u);
}

TEST_CASE(files_directories_and_links_persist)
{
    MemoryStore store(2048);
    {
        auto volume = MUST(Volume::format(store, "Data"_sv, s_uuid, test_clock));
        auto users = MUST(volume->create(root_directory, "Users"_sv, mode_directory | 0755, 0, 0));
        auto home = MUST(volume->create(users, "krisna"_sv, mode_directory | 0750, 501, 20));
        auto notes = MUST(volume->create(home, "notes.txt"_sv, mode_regular | 0644, 501, 20));
        MUST(volume->write(notes, 0, text("Hello from vultureFS\n")));
        MUST(volume->create(home, "latest"_sv, mode_symbolic_link | 0777, 501, 20, "notes.txt"_sv));
        MUST(volume->link(users, "shared-notes.txt"_sv, notes));
        EXPECT(volume->create(home, "notes.txt"_sv, mode_regular, 0, 0).error().code() == EEXIST);
        EXPECT(volume->link(root_directory, "home"_sv, home).error().code() == EPERM);
        MUST(volume->checkpoint());
        EXPECT_EQ(MUST(volume->inode(root_directory)).link_count, 3u);
        EXPECT_EQ(MUST(volume->inode(users)).link_count, 3u);
        EXPECT_EQ(MUST(volume->inode(notes)).link_count, 2u);
    }
    expect_clean(store);

    auto volume = MUST(Volume::mount(store, test_clock));
    auto users = MUST(volume->lookup(root_directory, "Users"_sv));
    EXPECT_EQ(users.type, type_directory);
    auto home = MUST(volume->lookup(users.inode, "krisna"_sv));
    auto home_record = MUST(volume->inode(home.inode));
    EXPECT_EQ(home_record.mode, mode_directory | 0750);
    EXPECT_EQ(home_record.uid, 501u);
    EXPECT_EQ(home_record.parent, users.inode);
    EXPECT_EQ(MUST(volume->lookup(home.inode, ".."_sv)).inode, users.inode);
    auto notes = MUST(volume->lookup(home.inode, "notes.txt"_sv));
    EXPECT(equals(read_all(*volume, notes.inode), text("Hello from vultureFS\n")));
    EXPECT_EQ(MUST(volume->lookup(users.inode, "shared-notes.txt"_sv)).inode, notes.inode);

    auto latest = MUST(volume->lookup(home.inode, "latest"_sv));
    EXPECT_EQ(latest.type, type_symbolic_link);
    char target[64];
    auto length = MUST(volume->read_link(latest.inode, { reinterpret_cast<u8*>(target), sizeof(target) }));
    EXPECT(StringView(target, length) == "notes.txt"_sv);

    size_t entries = 0;
    bool saw_notes = false;
    MUST(volume->for_each_entry(home.inode, [&](StringView name, DirectoryEntry entry) {
        ++entries;
        saw_notes = saw_notes || (name == "notes.txt"_sv && entry.inode == notes.inode);
        return Iteration::Continue;
    }));
    EXPECT_EQ(entries, 2u);
    EXPECT(saw_notes);

    // Unlinking one name keeps the other.
    auto unlinked = MUST(volume->unlink(users.inode, "shared-notes.txt"_sv));
    EXPECT(!unlinked.last_link);
    EXPECT_EQ(MUST(volume->inode(notes.inode)).link_count, 1u);
    EXPECT(volume->remove_directory(users.inode, "krisna"_sv).error().code() == ENOTEMPTY);
    EXPECT(volume->unlink(users.inode, "krisna"_sv).error().code() == EISDIR);
    MUST(volume->checkpoint());
    expect_clean(store);
}

TEST_CASE(large_sparse_and_truncated_files)
{
    MemoryStore store(4096);
    auto volume = MUST(Volume::format(store, "Data"_sv, s_uuid, test_clock));
    auto file = MUST(volume->create(root_directory, "big"_sv, mode_regular | 0644, 0, 0));

    // 1 MiB written in odd-sized pieces.
    FastRandom random(7);
    Vector<u8> expected;
    MUST(expected.try_resize(1024 * 1024));
    for (auto& byte : expected)
        byte = static_cast<u8>(random.next());
    for (size_t offset = 0; offset < expected.size();) {
        size_t length = min<size_t>(random.next_between(1, 70000), expected.size() - offset);
        MUST(volume->write(file, offset, { expected.begin() + offset, length }));
        offset += length;
    }
    EXPECT(equals(read_all(*volume, file), { expected.begin(), expected.size() }));
    // Overwrite a range in the middle; reads straddling blocks.
    MUST(volume->write(file, 5000, text("overwritten")));
    __builtin_memcpy(expected.begin() + 5000, "overwritten", 11);
    u8 piece[9000];
    EXPECT_EQ(MUST(volume->read(file, 4000, piece)), sizeof(piece));
    EXPECT(__builtin_memcmp(piece, expected.begin() + 4000, sizeof(piece)) == 0);
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT_EQ(MUST(volume->inode(file)).allocated_blocks, 256u);

    // A hole: writing far past the end leaves zeros between.
    auto sparse = MUST(volume->create(root_directory, "sparse"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->write(sparse, 10 * 1024 * 1024, text("end")));
    auto record = MUST(volume->inode(sparse));
    EXPECT_EQ(record.size, 10u * 1024 * 1024 + 3);
    EXPECT_EQ(record.allocated_blocks, 1u);
    u8 hole[100];
    __builtin_memset(hole, 0xaa, sizeof(hole));
    EXPECT_EQ(MUST(volume->read(sparse, 5 * 1024 * 1024, hole)), sizeof(hole));
    bool zeros = true;
    for (u8 byte : hole)
        zeros = zeros && byte == 0;
    EXPECT(zeros);

    // Truncating to the middle of a block clears the rest of it, so
    // growing again shows zeros, not the old bytes.
    MUST(volume->truncate(file, 10000));
    EXPECT_EQ(MUST(volume->inode(file)).allocated_blocks, 3u);
    MUST(volume->truncate(file, 20000));
    u8 regrown[10000];
    EXPECT_EQ(MUST(volume->read(file, 10000, regrown)), sizeof(regrown));
    zeros = true;
    for (u8 byte : regrown)
        zeros = zeros && byte == 0;
    EXPECT(zeros);
    u8 kept[100];
    EXPECT_EQ(MUST(volume->read(file, 9900, kept)), sizeof(kept));
    EXPECT(__builtin_memcmp(kept, expected.begin() + 9900, sizeof(kept)) == 0);
    MUST(volume->truncate(file, 0));
    EXPECT_EQ(MUST(volume->inode(file)).allocated_blocks, 0u);
    MUST(volume->checkpoint());
    expect_clean(store);
}

TEST_CASE(thousands_of_files_grow_and_shrink_the_tree)
{
    MemoryStore store(8192);
    auto volume = MUST(Volume::format(store, "Data"_sv, s_uuid, test_clock));
    InodeNumber directories[8];
    for (size_t d = 0; d < 8; ++d) {
        char name[16];
        auto length = format_to_buffer(name, "dir{}", d);
        directories[d] = MUST(volume->create(root_directory, { name, length }, mode_directory | 0755, 0, 0));
    }
    for (size_t i = 0; i < 4000; ++i) {
        char name[64];
        auto length = format_to_buffer(name, "a-rather-long-file-name-number-{}.txt", i);
        auto file = MUST(volume->create(directories[i % 8], { name, length }, mode_regular | 0644, 0, 0));
        if (i % 10 == 0)
            MUST(volume->write(file, 0, { reinterpret_cast<u8 const*>(name), length }));
        if (i % 1000 == 999)
            MUST(volume->checkpoint());
    }
    MUST(volume->checkpoint());
    auto grown = MUST(check_volume(store));
    EXPECT(grown.is_clean());
    EXPECT_EQ(grown.files, 4000u);
    EXPECT(grown.tree_height >= 3);
    EXPECT_EQ(MUST(volume->inode(directories[3])).size, 500u);

    // Read some back after forgetting the cached tree.
    {
        auto again = MUST(Volume::mount(store, test_clock));
        auto entry = MUST(again->lookup(directories[2], "a-rather-long-file-name-number-1210.txt"_sv));
        EXPECT(equals(read_all(*again, entry.inode), text("a-rather-long-file-name-number-1210.txt")));
    }

    for (size_t i = 0; i < 4000; ++i) {
        char name[64];
        auto length = format_to_buffer(name, "a-rather-long-file-name-number-{}.txt", i);
        auto unlinked = MUST(volume->unlink(directories[i % 8], { name, length }));
        EXPECT(unlinked.last_link);
        MUST(volume->release(unlinked.inode));
        if (i % 1500 == 0)
            MUST(volume->checkpoint());
    }
    for (size_t d = 0; d < 8; ++d) {
        char name[16];
        auto length = format_to_buffer(name, "dir{}", d);
        MUST(volume->release(MUST(volume->remove_directory(root_directory, { name, length })).inode));
    }
    MUST(volume->checkpoint());
    auto shrunk = MUST(check_volume(store));
    EXPECT(shrunk.is_clean());
    EXPECT_EQ(shrunk.files, 0u);
    EXPECT_EQ(shrunk.tree_height, 1u);
    // Superblocks, one bitmap block and the root leaf.
    EXPECT_EQ(shrunk.used_blocks, 4u);
}

TEST_CASE(rename_follows_posix)
{
    MemoryStore store(1024);
    auto volume = MUST(Volume::format(store, "Data"_sv, s_uuid, test_clock));
    auto a = MUST(volume->create(root_directory, "a"_sv, mode_directory | 0755, 0, 0));
    auto b = MUST(volume->create(root_directory, "b"_sv, mode_directory | 0755, 0, 0));
    auto inner = MUST(volume->create(a, "inner"_sv, mode_directory | 0755, 0, 0));
    auto one = MUST(volume->create(a, "one"_sv, mode_regular | 0644, 0, 0));
    auto two = MUST(volume->create(b, "two"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->write(two, 0, text("two")));

    // A file replaces a file; the replaced one is returned for release.
    auto replaced = MUST(volume->rename(a, "one"_sv, b, "two"_sv));
    EXPECT(replaced.has_value() && replaced->inode == two && replaced->last_link);
    MUST(volume->release(two));
    EXPECT_EQ(MUST(volume->lookup(b, "two"_sv)).inode, one);
    EXPECT(volume->lookup(a, "one"_sv).error().code() == ENOENT);

    // A directory moves between parents: link counts follow.
    EXPECT(!MUST(volume->rename(a, "inner"_sv, b, "inner"_sv)).has_value());
    EXPECT_EQ(MUST(volume->inode(a)).link_count, 2u);
    EXPECT_EQ(MUST(volume->inode(b)).link_count, 3u);
    EXPECT_EQ(MUST(volume->inode(inner)).parent, b);

    // Not into itself, not over a non-empty directory, not across kinds.
    EXPECT(volume->rename(root_directory, "b"_sv, inner, "b"_sv).error().code() == EINVAL);
    EXPECT(volume->rename(root_directory, "a"_sv, root_directory, "b"_sv).error().code() == ENOTEMPTY);
    EXPECT(volume->rename(b, "two"_sv, b, "inner"_sv).error().code() == EISDIR);
    EXPECT(volume->rename(b, "inner"_sv, b, "two"_sv).error().code() == ENOTDIR);

    // A directory over an empty one.
    auto empty = MUST(volume->create(root_directory, "empty"_sv, mode_directory | 0755, 0, 0));
    auto gone = MUST(volume->rename(b, "inner"_sv, root_directory, "empty"_sv));
    EXPECT(gone.has_value() && gone->inode == empty);
    MUST(volume->release(empty));
    EXPECT_EQ(MUST(volume->inode(inner)).parent, root_directory);
    EXPECT_EQ(MUST(volume->inode(root_directory)).link_count, 5u);

    // Renaming onto another name of the same file does nothing.
    MUST(volume->link(b, "also-two"_sv, one));
    EXPECT(!MUST(volume->rename(b, "also-two"_sv, b, "two"_sv)).has_value());
    EXPECT_EQ(MUST(volume->inode(one)).link_count, 2u);
    MUST(volume->checkpoint());
    expect_clean(store);
}

TEST_CASE(orphans_are_deleted_by_the_next_mount)
{
    MemoryStore store(1024);
    InodeNumber file;
    {
        auto volume = MUST(Volume::format(store, "Data"_sv, s_uuid, test_clock));
        file = MUST(volume->create(root_directory, "open-file"_sv, mode_regular | 0644, 0, 0));
        Vector<u8> data;
        MUST(data.try_resize(100000));
        MUST(volume->write(file, 0, { data.begin(), data.size() }));
        MUST(volume->checkpoint());
        // Unlinked while still open: kept, as an orphan, until released.
        auto unlinked = MUST(volume->unlink(root_directory, "open-file"_sv));
        EXPECT(unlinked.last_link);
        EXPECT_EQ(read_all(*volume, file).size(), 100000u);
        MUST(volume->checkpoint());
        auto report = MUST(check_volume(store));
        EXPECT(report.is_clean());
        EXPECT_EQ(report.orphans, 1u);
        // ... and then the machine crashes before it is closed.
    }
    auto volume = MUST(Volume::mount(store, test_clock));
    EXPECT(volume->inode(file).error().code() == ENOENT);
    auto report = MUST(check_volume(store));
    EXPECT(report.is_clean());
    EXPECT_EQ(report.orphans, 0u);
    EXPECT_EQ(report.data_blocks, 0u);
}

TEST_CASE(uncommitted_changes_vanish_in_a_crash)
{
    MemoryStore store(1024);
    {
        auto volume = MUST(Volume::format(store, "Data"_sv, s_uuid, test_clock));
        auto kept = MUST(volume->create(root_directory, "kept"_sv, mode_regular | 0644, 0, 0));
        MUST(volume->write(kept, 0, text("committed")));
        MUST(volume->checkpoint());
        MUST(volume->write(kept, 0, text("COMMITTED, then changed")));
        MUST(volume->create(root_directory, "lost"_sv, mode_directory | 0755, 0, 0));
    }
    store.crash_losing_everything();
    expect_clean(store);
    auto volume = MUST(Volume::mount(store, test_clock));
    auto kept = MUST(volume->lookup(root_directory, "kept"_sv));
    EXPECT(equals(read_all(*volume, kept.inode), text("committed")));
    EXPECT(volume->lookup(root_directory, "lost"_sv).error().code() == ENOENT);
}

TEST_CASE(damage_is_found_and_mount_falls_back_a_checkpoint)
{
    MemoryStore store(1024);
    BlockIndex newest_root;
    {
        auto volume = MUST(Volume::format(store, "Data"_sv, s_uuid, test_clock));
        auto file = MUST(volume->create(root_directory, "first"_sv, mode_regular | 0644, 0, 0));
        MUST(volume->write(file, 0, text("one")));
        MUST(volume->checkpoint());
        MUST(volume->create(root_directory, "second"_sv, mode_regular | 0644, 0, 0));
        MUST(volume->checkpoint());
        newest_root = volume->superblock().tree_root;
    }
    expect_clean(store);
    store.corrupt(newest_root, 100);
    auto report = MUST(check_volume(store));
    EXPECT(!report.is_clean());
    // The checkpoint before is still whole.
    auto volume = MUST(Volume::mount(store, test_clock));
    EXPECT_EQ(volume->statistics().generation, 2u);
    EXPECT(volume->lookup(root_directory, "second"_sv).error().code() == ENOENT);
    auto first = MUST(volume->lookup(root_directory, "first"_sv));
    EXPECT(equals(read_all(*volume, first.inode), text("one")));
    // Writing on from there replaces the damaged checkpoint.
    MUST(volume->create(root_directory, "third"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->checkpoint());
    EXPECT_EQ(volume->statistics().generation, 3u);
    expect_clean(store);

    // Both superblocks damaged: nothing to mount.
    store.corrupt(0, 20);
    store.corrupt(1, 20);
    EXPECT(Volume::mount(store, test_clock).is_error());
    EXPECT(check_volume(store).is_error());
}

TEST_CASE(a_full_volume_reports_enospc_and_stays_consistent)
{
    MemoryStore store(512);
    auto volume = MUST(Volume::format(store, "Small"_sv, s_uuid, test_clock));
    auto file = MUST(volume->create(root_directory, "filler"_sv, mode_regular | 0644, 0, 0));
    Vector<u8> chunk;
    MUST(chunk.try_resize(64 * 1024));
    u64 offset = 0;
    for (;;) {
        auto result = volume->write(file, offset, { chunk.begin(), chunk.size() });
        if (result.is_error()) {
            EXPECT_EQ(result.error().code(), ENOSPC);
            break;
        }
        offset += chunk.size();
        MUST(volume->checkpoint());
    }
    EXPECT(offset > 0);
    MUST(volume->checkpoint());
    expect_clean(store);
    MUST(volume->truncate(file, 0));
    MUST(volume->checkpoint());
    expect_clean(store);
}

namespace {

constexpr size_t model_directories = 3;
constexpr size_t model_files = 6;

// What the volume should hold: a few directories of a few files.
struct Model {
    bool directory_exists[model_directories] {};
    bool file_exists[model_directories][model_files] {};
    Vector<u8> contents[model_directories][model_files];
};

void directory_name(size_t d, char (&buffer)[16], StringView& name)
{
    name = { buffer, format_to_buffer(buffer, "d{}", d) };
}

void file_name(size_t f, char (&buffer)[16], StringView& name)
{
    name = { buffer, format_to_buffer(buffer, "file-{}", f) };
}

// Applies one random change to the volume and, when it succeeds, to the
// model. Returns false if the volume failed (lost power).
bool random_change(Volume& volume, Model& model, FastRandom& random)
{
    size_t d = random.next_below(model_directories);
    size_t f = random.next_below(model_files);
    char directory_buffer[16], file_buffer[16];
    StringView directory, file;
    directory_name(d, directory_buffer, directory);
    file_name(f, file_buffer, file);

    if (!model.directory_exists[d]) {
        if (volume.create(root_directory, directory, mode_directory | 0755, 0, 0).is_error())
            return false;
        model.directory_exists[d] = true;
        return true;
    }
    auto directory_inode = MUST(volume.lookup(root_directory, directory)).inode;
    u64 choice = random.next_below(10);
    if (!model.file_exists[d][f]) {
        if (choice == 0) {
            // Remove the directory when it is empty.
            bool empty = true;
            for (size_t i = 0; i < model_files; ++i)
                empty = empty && !model.file_exists[d][i];
            if (!empty)
                return true;
            auto removed = volume.remove_directory(root_directory, directory);
            if (removed.is_error() || volume.release(removed.value().inode).is_error())
                return false;
            model.directory_exists[d] = false;
            return true;
        }
        if (volume.create(directory_inode, file, mode_regular | 0644, 0, 0).is_error())
            return false;
        model.file_exists[d][f] = true;
        model.contents[d][f].clear();
        return true;
    }
    auto inode = MUST(volume.lookup(directory_inode, file)).inode;
    auto& contents = model.contents[d][f];
    if (choice < 6) {
        size_t offset = random.next_below(40000);
        size_t length = random.next_between(1, 30000);
        Vector<u8> data;
        MUST(data.try_resize(length));
        for (auto& byte : data)
            byte = static_cast<u8>(random.next());
        if (volume.write(inode, offset, { data.begin(), data.size() }).is_error())
            return false;
        if (contents.size() < offset + length) {
            size_t old = contents.size();
            MUST(contents.try_resize(offset + length));
            for (size_t i = old; i < contents.size(); ++i)
                contents[i] = 0;
        }
        __builtin_memcpy(contents.begin() + offset, data.begin(), length);
        return true;
    }
    if (choice < 8) {
        size_t size = random.next_below(50000);
        if (volume.truncate(inode, size).is_error())
            return false;
        size_t old = contents.size();
        MUST(contents.try_resize(size));
        for (size_t i = old; i < size; ++i)
            contents[i] = 0;
        return true;
    }
    if (choice == 8) {
        auto unlinked = volume.unlink(directory_inode, file);
        if (unlinked.is_error() || volume.release(unlinked.value().inode).is_error())
            return false;
        model.file_exists[d][f] = false;
        return true;
    }
    // Rename to another name in another existing directory.
    size_t to_d = random.next_below(model_directories);
    size_t to_f = random.next_below(model_files);
    if (!model.directory_exists[to_d] || (to_d == d && to_f == f))
        return true;
    char to_directory_buffer[16], to_file_buffer[16];
    StringView to_directory, to_file;
    directory_name(to_d, to_directory_buffer, to_directory);
    file_name(to_f, to_file_buffer, to_file);
    auto to_directory_inode = MUST(volume.lookup(root_directory, to_directory)).inode;
    auto replaced = volume.rename(directory_inode, file, to_directory_inode, to_file);
    if (replaced.is_error())
        return false;
    if (replaced.value().has_value() && volume.release(replaced.value()->inode).is_error())
        return false;
    model.file_exists[to_d][to_f] = true;
    model.contents[to_d][to_f] = move(contents);
    model.file_exists[d][f] = false;
    model.contents[d][f] = {};
    return true;
}

void expect_matches(Volume& volume, Model const& model)
{
    size_t directories = 0;
    MUST(volume.for_each_entry(root_directory, [&](StringView, DirectoryEntry) {
        ++directories;
        return Iteration::Continue;
    }));
    size_t expected_directories = 0;
    for (size_t d = 0; d < model_directories; ++d) {
        char directory_buffer[16];
        StringView directory;
        directory_name(d, directory_buffer, directory);
        auto found = volume.lookup(root_directory, directory);
        EXPECT_EQ(found.is_error(), !model.directory_exists[d]);
        if (!model.directory_exists[d] || found.is_error())
            continue;
        ++expected_directories;
        size_t files = 0, expected_files = 0;
        MUST(volume.for_each_entry(found.value().inode, [&](StringView, DirectoryEntry) {
            ++files;
            return Iteration::Continue;
        }));
        for (size_t f = 0; f < model_files; ++f) {
            char file_buffer[16];
            StringView file;
            file_name(f, file_buffer, file);
            auto entry = volume.lookup(found.value().inode, file);
            EXPECT_EQ(entry.is_error(), !model.file_exists[d][f]);
            if (!model.file_exists[d][f] || entry.is_error())
                continue;
            ++expected_files;
            auto const& expected = model.contents[d][f];
            EXPECT(equals(read_all(volume, entry.value().inode), { expected.begin(), expected.size() }));
        }
        EXPECT_EQ(files, expected_files);
    }
    EXPECT_EQ(directories, expected_directories);
}

}

TEST_CASE(power_loss_at_any_moment_leaves_the_last_checkpoint)
{
    // Random changes and checkpoints; the power fails after a random number
    // of writes, losing, reordering and tearing whatever was not flushed.
    // Each time the volume must mount, pass the check and hold exactly
    // what its newest committed checkpoint held.
    MemoryStore store(2048);
    FastRandom random(2026);
    Model committed;
    OwnPtr<Volume> volume = MUST(Volume::format(store, "Crash"_sv, s_uuid, test_clock));
    u64 committed_generation = volume->statistics().generation;
    size_t crashes = 0, in_flight_commits = 0;

    for (size_t round = 0; round < 300; ++round) {
        Model working = committed;
        Optional<Model> in_flight;
        store.fail_after(random.next_below(400));
        for (size_t step = 0; step < 200 && !store.is_dead(); ++step) {
            if (!random_change(*volume, working, random))
                break;
            if (random.next_below(8) == 0) {
                in_flight = working;
                if (volume->checkpoint().is_error())
                    break;
                committed = working;
                committed_generation = volume->statistics().generation;
                in_flight.clear();
            }
        }
        if (!store.is_dead()) {
            // Survived the round: commit and carry on without a crash.
            store.never_fail();
            MUST(volume->checkpoint());
            committed = working;
            committed_generation = volume->statistics().generation;
            continue;
        }

        ++crashes;
        volume.clear();
        store.crash(random);
        auto report = MUST(check_volume(store));
        for (auto const& problem : report.problems) {
            Test::print(problem.view());
            Test::print("\n"_sv);
        }
        EXPECT(report.is_clean());
        volume = OwnPtr<Volume>(MUST(Volume::mount(store, test_clock)));
        u64 generation = volume->statistics().generation;
        if (in_flight.has_value() && generation > committed_generation) {
            // The checkpoint under way when the power went got its
            // superblock onto the disk.
            ++in_flight_commits;
            committed = in_flight.release_value();
        } else {
            EXPECT_EQ(generation, committed_generation);
        }
        committed_generation = generation;
        expect_matches(*volume, committed);
        expect_clean(store);
    }
    EXPECT(crashes > 100);
    Test::print("power_loss: "_sv);
    char line[64];
    Test::print({ line, format_to_buffer(line, "{} crashes, {} mid-checkpoint commits\n", crashes, in_flight_commits) });
}
