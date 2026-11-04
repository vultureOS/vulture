/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// Zip archives, as a Mac makes and opens them: stored or deflated members, found by the central directory at the end. Every
// member's checksum is checked on reading. Zip64, encryption and multiple-disk archives aren't here.

namespace Archive {

struct ZipEntry {
    // As the archive has it: use is_safe_member_path (Tar.h) before unpacking it anywhere.
    String path;
    bool is_directory { false };
    bool is_symbolic_link { false };
    u32 mode { 0 };
    u32 compressed_size { 0 };
    u32 size { 0 };
    u32 crc32 { 0 };
    // From MS-DOS's clock, in the archive's own way: 2-second steps, no time zone.
    u16 dos_date { 0 };
    u16 dos_time { 0 };
    u16 method { 0 };
    u32 local_header_offset { 0 };
};

class ZipReader {
public:
    [[nodiscard]] static ErrorOr<ZipReader> parse(ReadonlyBytes archive);

    [[nodiscard]] Vector<ZipEntry> const& entries() const { return m_entries; }
    // Decompressed and checked.
    [[nodiscard]] ErrorOr<Vector<u8>> read(ZipEntry const&) const;

private:
    ZipReader() = default;

    ReadonlyBytes m_archive;
    Vector<ZipEntry> m_entries;
};

class ZipWriter {
public:
    // A directory's path ends with a '/'.
    ErrorOr<void> add_directory(StringView path, u32 mode, u16 dos_date, u16 dos_time);
    // Deflated, unless that doesn't make it smaller.
    ErrorOr<void> add_file(StringView path, u32 mode, ReadonlyBytes data, u16 dos_date, u16 dos_time);
    ErrorOr<void> add_symbolic_link(StringView path, StringView target, u16 dos_date, u16 dos_time);
    // The whole archive.
    [[nodiscard]] ErrorOr<Vector<u8>> finish();

private:
    ErrorOr<void> add(StringView path, u32 unix_mode, ReadonlyBytes data, bool may_compress, u16 dos_date, u16 dos_time);

    Vector<u8> m_body;
    Vector<u8> m_directory;
    u16 m_count { 0 };
};

}
