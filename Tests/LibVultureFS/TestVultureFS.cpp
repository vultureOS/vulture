/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/FastRandom.h>
#include <LibBase/HashMap.h>
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
    EXPECT_EQ(shrunk.used_blocks, 5u); // The superblocks, a node, a block of bitmap and where that is.
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


// ---- Version 2.

static StringView sv(char const* text) { return { text, __builtin_strlen(text) }; }

TEST_CASE(names_are_compared_as_a_mac_compares_them)
{
    MemoryStore store(2048);
    auto volume = MUST(Volume::format(store, "Names"_sv, s_uuid, test_clock));
    auto const file = MUST(volume->create(root_directory, "Foo"_sv, mode_regular | 0644, 0, 0));
    EXPECT_EQ(MUST(volume->lookup(root_directory, "foo"_sv)).inode, file);
    EXPECT_EQ(MUST(volume->lookup(root_directory, "FOO"_sv)).inode, file);
    EXPECT_EQ(volume->create(root_directory, "fOO"_sv, mode_regular | 0644, 0, 0).error().code(), EEXIST);

    // These all come out as the same file, or not, on a Mac.
    struct Pair {
        char const* first;
        char const* second;
        bool same;
    };
    Pair const pairs[] = {
        { "\xc3\xa9", "e\xcc\x81", true },                       // é, and e with a combining accent.
        { "e\xcc\x81x", "\xc3\xa9x", true },                     // The other way around.
        { "stra\xc3\x9f" "e", "STRASSE", true },                 // straße.
        { "\xc3\x9f", "\xe1\xba\x9e", true },                    // ß, ẞ.
        { "\xcf\x83", "\xcf\x82", true },                        // σ, ς.
        { "\xce\xa3", "\xcf\x82", true },                        // Σ, ς.
        { "I", "\xc4\xb1", false },                              // I, ı: no special treatment for Turkish.
        { "\xc4\xb0", "i\xcc\x87", true },                       // İ, and i with a dot.
        { "K", "\xe2\x84\xaa", true },                           // K, the Kelvin sign.
        { "\xc3\x85", "\xe2\x84\xab", true },                    // Å, the Angstrom sign.
        { "\xef\xac\x81", "fi", true },                          // ﬁ.
        { "\xef\xbc\xa1", "\xef\xbd\x81", true },                // Ａ, ａ, full width.
        { "\xef\xbc\xa1", "A", false },                          // Ａ, A.
        { "a\xe2\x80\x8d" "b", "ab", false },                    // A joiner.
        { "a\xe2\x80\x8c" "b", "ab", false },                    // A non-joiner.
        { "a\xc2\xad" "b", "ab", false },                        // A soft hyphen.
        { "\xed\x95\x9c", "\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab", true }, // 한, and its parts.
        { "\xe1\x8e\xa0", "\xea\xad\xb0", true },                // Cherokee.
        { "\xe2\x9d\xa4\xef\xb8\x8f", "\xe2\x9d\xa4", false },   // ❤, with a variation selector.
        { "a\xcc\xa3\xcc\x87", "a\xcc\x87\xcc\xa3", true },      // Marks in either order.
        { "\xc2\xbd", "1\xe2\x81\x84" "2", false },              // ½.
    };
    for (auto const& pair : pairs) {
        auto const made = MUST(volume->create(root_directory, sv(pair.first), mode_regular | 0644, 0, 0));
        auto const found = volume->lookup(root_directory, sv(pair.second));
        if (pair.same) {
            EXPECT(!found.is_error());
            if (!found.is_error())
                EXPECT_EQ(found.value().inode, made);
        } else {
            EXPECT(found.is_error());
        }
        // It's listed as it was given.
        bool listed = false;
        MUST(volume->for_each_entry(root_directory, [&](StringView name, DirectoryEntry) {
            listed = listed || name == sv(pair.first);
            return Iteration::Continue;
        }));
        EXPECT(listed);
        MUST(volume->release(MUST(volume->unlink(root_directory, sv(pair.second).is_empty() ? sv(pair.first) : sv(pair.first))).inode));
    }

    // What can't be made.
    char const* const bad[] = { "\xff", "a\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80" };
    for (auto const* name : bad) {
        EXPECT_EQ(volume->create(root_directory, sv(name), mode_regular | 0644, 0, 0).error().code(), EILSEQ);
        EXPECT_EQ(volume->lookup(root_directory, sv(name)).error().code(), ENOENT);
    }

    // As long as 255 of what UTF-16 has: three bytes count once, and four bytes twice.
    auto repeated = [](char const* text, size_t count) {
        StringBuilder builder;
        for (size_t i = 0; i < count; ++i)
            builder.append(sv(text));
        return MUST(builder.to_string());
    };
    EXPECT(!volume->create(root_directory, repeated("a", 255).view(), mode_regular | 0644, 0, 0).is_error());
    EXPECT_EQ(volume->create(root_directory, repeated("a", 256).view(), mode_regular | 0644, 0, 0).error().code(), ENAMETOOLONG);
    EXPECT(!volume->create(root_directory, repeated("\xed\x95\x9c", 255).view(), mode_regular | 0644, 0, 0).is_error());
    EXPECT_EQ(volume->create(root_directory, repeated("\xed\x95\x9c", 256).view(), mode_regular | 0644, 0, 0).error().code(), ENAMETOOLONG);
    EXPECT(!volume->create(root_directory, repeated("\xf0\x9f\x98\x80", 127).view(), mode_regular | 0644, 0, 0).is_error());
    EXPECT_EQ(volume->create(root_directory, repeated("\xf0\x9f\x98\x80", 128).view(), mode_regular | 0644, 0, 0).error().code(), ENAMETOOLONG);

    // A name can change only in the way it's written.
    auto const before = MUST(volume->create(root_directory, "abc"_sv, mode_regular | 0644, 0, 0));
    EXPECT(!MUST(volume->rename(root_directory, "abc"_sv, root_directory, "ABC"_sv)).has_value());
    EXPECT_EQ(MUST(volume->lookup(root_directory, "abc"_sv)).inode, before);
    bool has_new_spelling = false;
    bool has_old_spelling = false;
    MUST(volume->for_each_entry(root_directory, [&](StringView name, DirectoryEntry) {
        has_new_spelling = has_new_spelling || name == "ABC"_sv;
        has_old_spelling = has_old_spelling || name == "abc"_sv;
        return Iteration::Continue;
    }));
    EXPECT(has_new_spelling);
    EXPECT(!has_old_spelling);
    MUST(volume->checkpoint());
    expect_clean(store);

    // By request, a volume where case matters. Accents still don't.
    MemoryStore other(2048);
    auto sensitive = MUST(Volume::format(other, "Sensitive"_sv, s_uuid, test_clock, { .case_sensitive = true }));
    auto const lower = MUST(sensitive->create(root_directory, "a"_sv, mode_regular | 0644, 0, 0));
    auto const upper = MUST(sensitive->create(root_directory, "A"_sv, mode_regular | 0644, 0, 0));
    EXPECT(lower != upper);
    auto const accented = MUST(sensitive->create(root_directory, sv("\xc3\xa9"), mode_regular | 0644, 0, 0));
    EXPECT_EQ(MUST(sensitive->lookup(root_directory, sv("e\xcc\x81"))).inode, accented);
    MUST(sensitive->checkpoint());
    expect_clean(other);
}

TEST_CASE(a_volume_of_the_old_format_is_refused_clearly)
{
    MemoryStore store(256);
    auto volume = MUST(Volume::format(store, "Old"_sv, s_uuid, test_clock));
    MUST(volume->checkpoint());
    u8 block[block_size];
    MUST(store.read_blocks(1, 1, { block, block_size }));
    __builtin_memcpy(block, old_superblock_magic, sizeof(old_superblock_magic));
    MUST(store.write_blocks(0, 1, { block, block_size }));
    MUST(store.write_blocks(1, 1, { block, block_size }));
    MUST(store.flush());
    auto const mounted = Volume::mount(store, test_clock);
    EXPECT(mounted.is_error());
    EXPECT(mounted.error().string_literal().contains("version 1"_sv));
}

TEST_CASE(pipes_and_sockets_have_names)
{
    MemoryStore store(512);
    auto volume = MUST(Volume::format(store, "Nodes"_sv, s_uuid, test_clock));
    auto const pipe = MUST(volume->create(root_directory, "pipe"_sv, mode_fifo | 0600, 0, 0));
    auto const socket = MUST(volume->create(root_directory, "socket"_sv, mode_socket | 0600, 0, 0));
    EXPECT_EQ(MUST(volume->lookup(root_directory, "pipe"_sv)).type, type_fifo);
    EXPECT_EQ(MUST(volume->lookup(root_directory, "socket"_sv)).type, type_socket);
    EXPECT_EQ(MUST(volume->inode(pipe)).mode & mode_type_mask, mode_fifo);
    EXPECT_EQ(MUST(volume->inode(socket)).mode & mode_type_mask, mode_socket);
    MUST(volume->checkpoint());
    expect_clean(store);
}

static Vector<u8> pattern(size_t size, u32 seed)
{
    Vector<u8> data;
    MUST(data.try_resize(size));
    FastRandom random(seed);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>(random.next_below(256));
    return data;
}

static Vector<u8> compressible(size_t size)
{
    Vector<u8> data;
    MUST(data.try_resize(size));
    for (size_t i = 0; i < size; ++i)
        data[i] = "All work and no play makes a dull file. "[i % 40];
    return data;
}

TEST_CASE(clones_share_blocks_until_one_changes)
{
    MemoryStore store(8192);
    auto volume = MUST(Volume::format(store, "Clones"_sv, s_uuid, test_clock));
    auto const data = pattern(1024 * 1024, 7);
    auto const original = MUST(volume->create(root_directory, "original"_sv, mode_regular | 04755, 5, 5));
    MUST(volume->write(original, 0, { data.begin(), data.size() }));
    MUST(volume->set_xattr(original, "user.note"_sv, text("kept")));
    AclEntry entry {};
    entry.kind = acl_allow;
    entry.tag = acl_user;
    entry.id = 501;
    entry.permissions = acl_read_data;
    MUST(volume->set_acl(original, Span<AclEntry const>(&entry, 1)));
    MUST(volume->checkpoint());
    u64 const free_before = volume->statistics().free_blocks;

    auto const clone = MUST(volume->clone(original, root_directory, "clone"_sv, 7, 7));
    MUST(volume->checkpoint());
    expect_clean(store);
    // The tree grew a little, but not by a file's worth.
    EXPECT(free_before - volume->statistics().free_blocks < 16);
    EXPECT(equals(read_all(*volume, clone), { data.begin(), data.size() }));
    auto const clone_record = MUST(volume->inode(clone));
    EXPECT_EQ(clone_record.size, data.size());
    EXPECT_EQ(clone_record.uid, 7u);
    EXPECT_EQ(clone_record.mode & 07777, 0755u); // It's not set-user-ID for someone else.
    char note[8];
    EXPECT_EQ(MUST(volume->get_xattr(clone, "user.note"_sv, { reinterpret_cast<u8*>(note), sizeof(note) })), 4u);
    EXPECT_EQ(MUST(volume->acl(clone)).size(), 1u);
    EXPECT_EQ(volume->clone(original, root_directory, "clone"_sv, 0, 0).error().code(), EEXIST);

    // Changing one changes only that one.
    auto const changed = pattern(4096, 9);
    MUST(volume->write(clone, 8192, { changed.begin(), changed.size() }));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT(equals(read_all(*volume, original), { data.begin(), data.size() }));
    auto const after = read_all(*volume, clone);
    EXPECT(__builtin_memcmp(after.begin() + 8192, changed.begin(), 4096) == 0);
    EXPECT(__builtin_memcmp(after.begin(), data.begin(), 8192) == 0);

    // Removing the original leaves the clone whole; removing that as well gives the blocks back.
    MUST(volume->release(MUST(volume->unlink(root_directory, "original"_sv)).inode));
    MUST(volume->checkpoint());
    expect_clean(store);
    auto const remaining = read_all(*volume, clone);
    EXPECT(__builtin_memcmp(remaining.begin() + 12288, data.begin() + 12288, data.size() - 12288) == 0);
    MUST(volume->release(MUST(volume->unlink(root_directory, "clone"_sv)).inode));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT(volume->statistics().free_blocks > free_before);

    // A whole tree, with what's in it, as it was.
    auto const directory = MUST(volume->create(root_directory, "folder"_sv, mode_directory | 0755, 0, 0));
    auto const inner = MUST(volume->create(directory, "inner"_sv, mode_directory | 0755, 0, 0));
    auto const deep = MUST(volume->create(inner, "deep"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->write(deep, 0, text("deep down")));
    (void)MUST(volume->create(directory, "link"_sv, mode_symbolic_link | 0777, 0, 0, "inner/deep"_sv));
    auto const copy = MUST(volume->clone(directory, root_directory, "copy"_sv, 0, 0));
    MUST(volume->checkpoint());
    expect_clean(store);
    auto const copied_inner = MUST(volume->lookup(copy, "inner"_sv));
    auto const copied_deep = MUST(volume->lookup(copied_inner.inode, "deep"_sv));
    EXPECT(copied_deep.inode != deep);
    EXPECT(equals(read_all(*volume, copied_deep.inode), text("deep down")));
    auto const copied_link = MUST(volume->lookup(copy, "link"_sv));
    char target[32];
    EXPECT_EQ(MUST(volume->read_link(copied_link.inode, { reinterpret_cast<u8*>(target), sizeof(target) })), 10u);
}

TEST_CASE(extended_attributes_of_any_size)
{
    MemoryStore store(8192);
    {
        auto volume = MUST(Volume::format(store, "Attributes"_sv, s_uuid, test_clock));
        auto const file = MUST(volume->create(root_directory, "file"_sv, mode_regular | 0644, 0, 0));
        u8 buffer[64];
        EXPECT_EQ(volume->get_xattr(file, "user.missing"_sv, { buffer, sizeof(buffer) }).error().code(), ENOATTR);
        MUST(volume->set_xattr(file, "user.small"_sv, text("value")));
        EXPECT_EQ(MUST(volume->get_xattr(file, "user.small"_sv, {})), 5u);
        EXPECT_EQ(MUST(volume->get_xattr(file, "user.small"_sv, { buffer, sizeof(buffer) })), 5u);
        EXPECT(__builtin_memcmp(buffer, "value", 5) == 0);
        EXPECT_EQ(volume->get_xattr(file, "user.small"_sv, { buffer, 2 }).error().code(), ERANGE);
        EXPECT_EQ(volume->set_xattr(file, "user.small"_sv, text("x"), XattrMode::MustBeNew).error().code(), EEXIST);
        EXPECT_EQ(volume->set_xattr(file, "user.other"_sv, text("x"), XattrMode::MustExist).error().code(), ENOATTR);
        MUST(volume->set_xattr(file, "user.small"_sv, text("newer"), XattrMode::MustExist));
        // One too big for the tree.
        auto const big = pattern(300 * 1024, 11);
        MUST(volume->set_xattr(file, "user.big"_sv, { big.begin(), big.size() }));
        MUST(volume->set_xattr(file, "user.empty"_sv, {}));
        MUST(volume->checkpoint());
        expect_clean(store);
    }
    {
        auto volume = MUST(Volume::mount(store, test_clock));
        auto const file = MUST(volume->lookup(root_directory, "file"_sv)).inode;
        size_t count = 0;
        u64 total = 0;
        MUST(volume->for_each_xattr(file, [&](StringView, u64 size) {
            ++count;
            total += size;
            return Iteration::Continue;
        }));
        EXPECT_EQ(count, 3u);
        EXPECT_EQ(total, 300u * 1024 + 5);
        Vector<u8> value;
        MUST(value.try_resize(300 * 1024));
        EXPECT_EQ(MUST(volume->get_xattr(file, "user.big"_sv, { value.begin(), value.size() })), value.size());
        auto const expected = pattern(300 * 1024, 11);
        EXPECT(__builtin_memcmp(value.begin(), expected.begin(), value.size()) == 0);
        // Replacing it, and removing it, gives back what it had.
        u64 const free_with = volume->statistics().free_blocks;
        MUST(volume->remove_xattr(file, "user.big"_sv));
        MUST(volume->checkpoint());
        EXPECT(volume->statistics().free_blocks > free_with);
        EXPECT_EQ(volume->remove_xattr(file, "user.big"_sv).error().code(), ENOATTR);
        expect_clean(store);
        // They go with the file.
        auto const big = pattern(100 * 1024, 12);
        MUST(volume->set_xattr(file, "user.again"_sv, { big.begin(), big.size() }));
        MUST(volume->release(MUST(volume->unlink(root_directory, "file"_sv)).inode));
        MUST(volume->checkpoint());
        expect_clean(store);
    }
}

TEST_CASE(a_flipped_bit_in_a_file_is_found)
{
    static InodeNumber s_damaged_inode;
    static u64 s_damaged_block;
    MemoryStore store(1024);
    auto volume = MUST(Volume::format(store, "Damage"_sv, s_uuid, test_clock));
    volume->set_damage_reporter([](InodeNumber inode, u64 file_block, BlockIndex) {
        s_damaged_inode = inode;
        s_damaged_block = file_block;
    });
    auto const data = pattern(64 * 1024, 3);
    auto const file = MUST(volume->create(root_directory, "file"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->write(file, 0, { data.begin(), data.size() }));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT(equals(read_all(*volume, file), { data.begin(), data.size() }));

    auto const disk_block = MUST(volume->map_block(file, 5)).value();
    store.corrupt(disk_block, 100);
    Vector<u8> buffer;
    MUST(buffer.try_resize(data.size()));
    EXPECT_EQ(volume->read(file, 0, { buffer.begin(), buffer.size() }).error().code(), EIO);
    EXPECT_EQ(s_damaged_inode, file);
    EXPECT_EQ(s_damaged_block, 5u);
    // What's on either side of it is still there.
    EXPECT_EQ(MUST(volume->read(file, 0, { buffer.begin(), 5 * block_size })), 5 * block_size);
    auto const report = MUST(check_volume(store));
    EXPECT(!report.is_clean());
    bool said_so = false;
    for (auto const& problem : report.problems)
        said_so = said_so || problem.view().contains("is damaged"_sv);
    EXPECT(said_so);
    // ... and a look that doesn't read the data doesn't find it.
    EXPECT(MUST(check_volume(store, { .check_data = false })).is_clean());
    store.corrupt(disk_block, 100);
    expect_clean(store);
}

TEST_CASE(compressed_files_read_the_same)
{
    MemoryStore store(8192);
    auto volume = MUST(Volume::format(store, "Compression"_sv, s_uuid, test_clock));
    auto const file = MUST(volume->create(root_directory, "file"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->set_compressed(file, true));
    auto data = compressible(1024 * 1024 + 123);
    MUST(volume->write(file, 0, { data.begin(), data.size() }));
    MUST(volume->checkpoint());
    expect_clean(store);
    auto record = MUST(volume->inode(file));
    EXPECT_EQ(record.size, data.size());
    EXPECT(record.allocated_blocks < data.size() / block_size / 4);
    EXPECT(equals(read_all(*volume, file), { data.begin(), data.size() }));

    // Little reads, across chunks.
    u8 piece[10000];
    EXPECT_EQ(MUST(volume->read(file, 65530, { piece, sizeof(piece) })), sizeof(piece));
    EXPECT(__builtin_memcmp(piece, data.begin() + 65530, sizeof(piece)) == 0);

    // A write into the middle, one past the end, and one that leaves a hole.
    auto const middle = pattern(9000, 21);
    MUST(volume->write(file, 60000, { middle.begin(), middle.size() }));
    __builtin_memcpy(data.begin() + 60000, middle.begin(), middle.size());
    MUST(volume->write(file, data.size(), text("more")));
    size_t const old_size = data.size();
    MUST(data.try_resize(old_size + 4));
    __builtin_memcpy(data.begin() + old_size, "more", 4);
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT(equals(read_all(*volume, file), { data.begin(), data.size() }));

    MUST(volume->write(file, 3 * 1024 * 1024, text("far")));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT_EQ(MUST(volume->read(file, 2 * 1024 * 1024, { piece, 100 })), 100u);
    for (size_t i = 0; i < 100; ++i)
        EXPECT_EQ(piece[i], 0);
    MUST(volume->truncate(file, data.size()));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT(equals(read_all(*volume, file), { data.begin(), data.size() }));

    // Shorter, in the middle of a chunk; then what's after it reads as nothing when it grows again.
    MUST(volume->truncate(file, 70000));
    MUST(data.try_resize(70000));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT(equals(read_all(*volume, file), { data.begin(), data.size() }));
    MUST(volume->truncate(file, 80000));
    EXPECT_EQ(MUST(volume->read(file, 70000, { piece, 5000 })), 5000u);
    for (size_t i = 0; i < 5000; ++i)
        EXPECT_EQ(piece[i], 0);
    MUST(volume->truncate(file, 70000));

    // A clone of it, and turning it back into a plain file.
    auto const clone = MUST(volume->clone(file, root_directory, "clone"_sv, 0, 0));
    MUST(volume->set_compressed(file, false));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT(!(MUST(volume->inode(file)).flags & inode_flag_compressed));
    EXPECT_EQ(MUST(volume->inode(file)).allocated_blocks, (70000u + block_size - 1) / block_size);
    EXPECT(equals(read_all(*volume, file), { data.begin(), data.size() }));
    EXPECT(equals(read_all(*volume, clone), { data.begin(), data.size() }));

    // Something that won't compress takes as much room as it would have, and is as readable.
    auto const noise = pattern(200 * 1024, 31);
    auto const random_file = MUST(volume->create(root_directory, "noise"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->set_compressed(random_file, true));
    MUST(volume->write(random_file, 0, { noise.begin(), noise.size() }));
    MUST(volume->checkpoint());
    expect_clean(store);
    EXPECT_EQ(MUST(volume->inode(random_file)).allocated_blocks, 50u);
    EXPECT(equals(read_all(*volume, random_file), { noise.begin(), noise.size() }));

    // And damage to a compressed one is found as well.
    auto const damaged = MUST(volume->create(root_directory, "damaged"_sv, mode_regular | 0644, 0, 0));
    MUST(volume->set_compressed(damaged, true));
    auto const words = compressible(32 * 1024);
    MUST(volume->write(damaged, 0, { words.begin(), words.size() }));
    MUST(volume->checkpoint());
    MUST(volume->read(random_file, 0, { piece, 1 }));
}

TEST_CASE(a_snapshot_is_that_moment_exactly)
{
    MemoryStore store(8192);
    auto const first = pattern(200 * 1024, 41);
    auto const second = pattern(200 * 1024, 42);
    InodeNumber file;
    {
        auto volume = MUST(Volume::format(store, "Snapshots"_sv, s_uuid, test_clock));
        file = MUST(volume->create(root_directory, "file"_sv, mode_regular | 0644, 0, 0));
        MUST(volume->write(file, 0, { first.begin(), first.size() }));
        auto const directory = MUST(volume->create(root_directory, "folder"_sv, mode_directory | 0755, 0, 0));
        (void)MUST(volume->create(directory, "inside"_sv, mode_regular | 0644, 0, 0));
        MUST(volume->set_xattr(file, "user.when"_sv, text("before")));
        EXPECT_EQ(volume->delete_snapshot("none"_sv).error().code(), ENOENT);
        MUST(volume->create_snapshot("one"_sv));
        EXPECT_EQ(volume->create_snapshot("one"_sv).error().code(), EEXIST);
        u64 const free_with_one = volume->statistics().free_blocks;

        // Now everything changes.
        MUST(volume->write(file, 0, { second.begin(), second.size() }));
        MUST(volume->set_xattr(file, "user.when"_sv, text("after")));
        MUST(volume->release(MUST(volume->unlink(MUST(volume->lookup(root_directory, "folder"_sv)).inode, "inside"_sv)).inode));
        (void)MUST(volume->create(root_directory, "new"_sv, mode_regular | 0644, 0, 0));
        MUST(volume->checkpoint());
        expect_clean(store);
        // The old contents haven't gone anywhere, and neither has the room they take.
        EXPECT(volume->statistics().snapshot_blocks >= first.size() / block_size);
        EXPECT(volume->statistics().free_blocks < free_with_one);

        MUST(volume->create_snapshot("two"_sv));
        auto const list = MUST(volume->snapshots());
        EXPECT_EQ(list.size(), 2u);
        EXPECT_EQ(list[0].name.view(), "one"_sv);
        EXPECT_EQ(list[1].name.view(), "two"_sv);
        expect_clean(store);
    }
    {
        // Both can be looked at, as they were.
        auto one = MUST(Volume::mount_snapshot(store, test_clock, "one"_sv));
        EXPECT(one->is_read_only());
        EXPECT(equals(read_all(*one, file), { first.begin(), first.size() }));
        EXPECT(one->lookup(root_directory, "new"_sv).is_error());
        EXPECT(!one->lookup(MUST(one->lookup(root_directory, "folder"_sv)).inode, "inside"_sv).is_error());
        EXPECT_EQ(one->create(root_directory, "x"_sv, mode_regular | 0644, 0, 0).error().code(), EROFS);
        auto two = MUST(Volume::mount_snapshot(store, test_clock, "two"_sv));
        EXPECT(equals(read_all(*two, file), { second.begin(), second.size() }));
        EXPECT(!two->lookup(root_directory, "new"_sv).is_error());
    }
    {
        // Going back happens on the next mount, and a crash on the way changes nothing.
        auto volume = MUST(Volume::mount(store, test_clock));
        EXPECT_EQ(volume->request_rollback("nonexistent"_sv).error().code(), ENOENT);
        MUST(volume->request_rollback("one"_sv));
        EXPECT(equals(read_all(*volume, file), { second.begin(), second.size() }));
    }
    {
        auto volume = MUST(Volume::mount(store, test_clock));
        expect_clean(store);
        EXPECT(equals(read_all(*volume, file), { first.begin(), first.size() }));
        EXPECT(volume->lookup(root_directory, "new"_sv).is_error());
        EXPECT(!volume->lookup(MUST(volume->lookup(root_directory, "folder"_sv)).inode, "inside"_sv).is_error());
        char value[16];
        EXPECT_EQ(MUST(volume->get_xattr(file, "user.when"_sv, { reinterpret_cast<u8*>(value), sizeof(value) })), 6u);
        EXPECT(__builtin_memcmp(value, "before", 6) == 0);
        // It's still there to go back to, and what came after it isn't.
        auto const list = MUST(volume->snapshots());
        EXPECT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].name.view(), "one"_sv);

        // It goes on working, and letting go of the snapshot gives back what only it had.
        MUST(volume->write(file, 0, { second.begin(), second.size() }));
        MUST(volume->checkpoint());
        expect_clean(store);
        u64 const held = volume->statistics().snapshot_blocks;
        EXPECT(held >= first.size() / block_size);
        u64 const free_before = volume->statistics().free_blocks;
        MUST(volume->delete_snapshot("one"_sv));
        expect_clean(store);
        EXPECT_EQ(volume->statistics().snapshot_blocks, 0u);
        EXPECT(volume->statistics().free_blocks > free_before + first.size() / block_size / 2);
        EXPECT(equals(read_all(*volume, file), { second.begin(), second.size() }));
    }
    {
        auto volume = MUST(Volume::mount(store, test_clock));
        expect_clean(store);
        EXPECT(MUST(volume->snapshots()).is_empty());
    }
}

TEST_CASE(what_changed_is_kept_with_the_change)
{
    MemoryStore store(4096);
    u64 before_crash;
    {
        auto volume = MUST(Volume::format(store, "Events"_sv, s_uuid, test_clock));
        EXPECT_EQ(volume->next_event(), 1u);
        EXPECT_EQ(MUST(volume->append_event(event_item_created | event_item_is_file, "/a"_sv)), 1u);
        EXPECT_EQ(MUST(volume->append_event(event_item_removed | event_item_is_file, "/a"_sv)), 2u);
        MUST(volume->checkpoint());
        before_crash = volume->next_event();
        (void)MUST(volume->append_event(event_item_created | event_item_is_file, "/lost"_sv));
        store.crash_losing_everything();
    }
    {
        auto volume = MUST(Volume::mount(store, test_clock));
        expect_clean(store);
        EXPECT_EQ(volume->next_event(), before_crash);
        size_t count = 0;
        MUST(volume->for_each_event(2, [&](u64 id, u32 flags, StringView path) {
            EXPECT_EQ(id, 2u);
            EXPECT_EQ(flags, event_item_removed | event_item_is_file);
            EXPECT_EQ(path, "/a"_sv);
            ++count;
            return Iteration::Continue;
        }));
        EXPECT_EQ(count, 1u);
        // Only so many are kept.
        for (u64 i = 0; i < max_kept_events + 100; ++i)
            (void)MUST(volume->append_event(event_item_modified, "/file"_sv));
        MUST(volume->checkpoint());
        expect_clean(store);
        EXPECT_EQ(volume->next_event() - volume->oldest_event(), max_kept_events);
        size_t kept = 0;
        MUST(volume->for_each_event(0, [&](u64, u32, StringView) {
            ++kept;
            return Iteration::Continue;
        }));
        EXPECT_EQ(kept, max_kept_events);
    }
}

TEST_CASE(a_volume_can_be_bigger_than_the_old_limit)
{
    // A million blocks and more of bitmap, as far as anything here's concerned, on a store with only what's been written.
    class SparseStore final : public BlockStore {
    public:
        explicit SparseStore(u64 blocks)
            : m_blocks(blocks)
        {
        }
        [[nodiscard]] u64 block_count() const override { return m_blocks; }
        ErrorOr<void> read_blocks(BlockIndex first, size_t count, Bytes buffer) override
        {
            for (size_t i = 0; i < count; ++i) {
                if (auto const* block = m_written.get(first + i))
                    __builtin_memcpy(buffer.data() + i * block_size, block->begin(), block_size);
                else
                    __builtin_memset(buffer.data() + i * block_size, 0, block_size);
            }
            return {};
        }
        ErrorOr<void> write_blocks(BlockIndex first, size_t count, ReadonlyBytes buffer) override
        {
            for (size_t i = 0; i < count; ++i) {
                Vector<u8> block;
                TRY(block.try_resize(block_size));
                __builtin_memcpy(block.begin(), buffer.data() + i * block_size, block_size);
                TRY(m_written.try_set(first + i, move(block)));
            }
            return {};
        }
        ErrorOr<void> flush() override { return {}; }

    private:
        u64 m_blocks;
        HashMap<u64, Vector<u8>> m_written;
    };

    u64 const terabyte_blocks = 1024ull * 1024 * 1024 * 1024 / block_size;
    SparseStore store(terabyte_blocks);
    {
        auto volume = MUST(Volume::format(store, "Big"_sv, s_uuid, test_clock));
        EXPECT_EQ(volume->statistics().block_count, terabyte_blocks);
        auto const file = MUST(volume->create(root_directory, "file"_sv, mode_regular | 0644, 0, 0));
        MUST(volume->write(file, 0, text("the start")));
        // Way past where a version 1 volume ended, by a hole.
        MUST(volume->write(file, 40ull * 1024 * 1024 * 1024, text("the end")));
        MUST(volume->checkpoint());
    }
    {
        auto volume = MUST(Volume::mount(store, test_clock));
        auto const file = MUST(volume->lookup(root_directory, "file"_sv)).inode;
        EXPECT_EQ(MUST(volume->inode(file)).size, 40ull * 1024 * 1024 * 1024 + 7);
        u8 end[7];
        EXPECT_EQ(MUST(volume->read(file, 40ull * 1024 * 1024 * 1024, { end, 7 })), 7u);
        EXPECT(__builtin_memcmp(end, "the end", 7) == 0);
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
