/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// The file systems other machines use: FAT, exFAT, ISO 9660. As with LibVultureFS, one implementation serves everything:
// it's built into the kernel (Kernel/FileSystem/ForeignFS.cpp), into the tools that format disks, and into tests on the
// host, which can then ask a real Mac's fsck what it thinks of the result.
namespace ForeignFS {

using NodeIndex = u64;

// Where a volume's sectors live: a partition in the kernel, an image file on the host, memory in the tests.
class Store {
public:
    static constexpr size_t sector_size = 512;

    virtual ~Store() = default;
    [[nodiscard]] virtual u64 sector_count() const = 0;
    [[nodiscard]] virtual bool is_read_only() const = 0;
    virtual ErrorOr<void> read_sectors(u64 first, size_t count, Bytes) = 0;
    virtual ErrorOr<void> write_sectors(u64 first, size_t count, ReadonlyBytes) = 0;
    virtual ErrorOr<void> flush() = 0;
};

// Seconds since 1970, in UTC.
using Clock = i64 (*)();

struct NodeInfo {
    NodeIndex index { 0 };
    u64 size { 0 };
    u64 allocated { 0 };
    bool is_directory { false };
    bool read_only { false };
    bool hidden { false };
    i64 created { 0 };
    i64 modified { 0 };
    i64 accessed { 0 };
};

struct Statistics {
    u64 block_size { 0 };
    u64 block_count { 0 };
    u64 free_blocks { 0 };
};

// Where a file lies on the store, for writing to in a panic, if that's all in one place.
struct Extent {
    u64 first_sector { 0 };
    u64 size { 0 };
};

enum class Iteration : u8 {
    Continue,
    Stop,
};

// Each of these is safe from one thread at a time: the kernel's side has a lock.
class Volume {
public:
    virtual ~Volume() = default;

    // What `mount` and `diskutil` call it: msdos, exfat, cd9660.
    [[nodiscard]] virtual StringView type_name() const = 0;
    [[nodiscard]] virtual StringView label() const = 0;
    [[nodiscard]] virtual bool is_read_only() const = 0;
    [[nodiscard]] virtual ErrorOr<Statistics> statistics() = 0;
    [[nodiscard]] virtual NodeIndex root() const = 0;

    virtual ErrorOr<NodeInfo> info(NodeIndex) = 0;
    virtual ErrorOr<NodeInfo> lookup(NodeIndex directory, StringView name) = 0;
    virtual ErrorOr<void> for_each_entry(NodeIndex directory, Function<Iteration(StringView name, NodeInfo const&)>) = 0;
    virtual ErrorOr<size_t> read(NodeIndex, u64 offset, Bytes) = 0;
    virtual ErrorOr<size_t> write(NodeIndex, u64 offset, ReadonlyBytes) = 0;
    virtual ErrorOr<void> truncate(NodeIndex, u64 size) = 0;
    virtual ErrorOr<NodeInfo> create(NodeIndex directory, StringView name, bool is_directory) = 0;
    // The name goes now. What it held goes when it's forgotten, as it may still be in use until then.
    virtual ErrorOr<void> remove(NodeIndex directory, StringView name) = 0;
    virtual ErrorOr<void> rename(NodeIndex from_directory, StringView from_name, NodeIndex to_directory, StringView to_name) = 0;
    virtual ErrorOr<void> set_times(NodeIndex, i64 accessed, i64 modified) = 0;
    virtual ErrorOr<void> set_read_only(NodeIndex, bool) = 0;
    // Nobody's using this any more. A file that's been removed gives back its space.
    virtual void forget(NodeIndex) = 0;
    // Everything's on the disk, and the volume says it was left in good order.
    virtual ErrorOr<void> flush() = 0;

    virtual ErrorOr<Extent> extent(NodeIndex) { return Error::from_errno(22); }

    // Sets right what's wrong, as far as that can be done without losing anything anyone could have got at. What it
    // did goes to `say`.
    virtual ErrorOr<void> repair(Function<void(StringView)>) { return Error::from_errno(30); }
};

enum class Kind : u8 {
    None,
    FAT,
    ExFAT,
    ISO9660,
};

// What's on a store, from the first few sectors of it.
[[nodiscard]] Kind recognise(Store&);
[[nodiscard]] StringView name_of(Kind);

[[nodiscard]] ErrorOr<NonnullOwnPtr<Volume>> open_volume(Store&, Clock, bool read_only);
[[nodiscard]] ErrorOr<NonnullOwnPtr<Volume>> open_fat(Store&, Clock, bool read_only);
[[nodiscard]] ErrorOr<NonnullOwnPtr<Volume>> open_exfat(Store&, Clock, bool read_only);
[[nodiscard]] ErrorOr<NonnullOwnPtr<Volume>> open_iso9660(Store&);

// Makes one. For FAT, `bits` says which kind (12, 16, 32), or 0 for whichever suits its size, as a Mac would choose.
ErrorOr<void> format_fat(Store&, StringView label, Clock, unsigned bits = 0);
ErrorOr<void> format_exfat(Store&, StringView label, Clock);

// What can be said about a volume without changing it, and what's the matter with it, line by line.
struct CheckReport {
    Vector<String> problems;
    u64 files { 0 };
    u64 directories { 0 };
    [[nodiscard]] bool is_clean() const { return problems.is_empty(); }
};
[[nodiscard]] ErrorOr<CheckReport> check_fat(Store&, Clock);
[[nodiscard]] ErrorOr<CheckReport> check_exfat(Store&, Clock);

[[nodiscard]] constexpr bool is_one_of(StringView set, u16 unit)
{
    for (size_t i = 0; i < set.length(); ++i) {
        if (static_cast<u16>(set[i]) == unit)
            return true;
    }
    return false;
}

// For names in UTF-16, which is how both of them keep them.
[[nodiscard]] ErrorOr<Vector<u16>> utf16_of(StringView utf8);
[[nodiscard]] ErrorOr<String> utf8_of_utf16(Span<u16 const>);
// What's compared: its capitals, in NFC, as it would be spelled on a Mac.
[[nodiscard]] ErrorOr<Vector<u16>> comparable_name(StringView utf8);

}
