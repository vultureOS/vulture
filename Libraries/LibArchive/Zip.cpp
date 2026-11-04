/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Zip.h>
#include <LibCompress/Deflate.h>

namespace Archive {

namespace {

constexpr u32 local_signature = 0x04034b50;
constexpr u32 central_signature = 0x02014b50;
constexpr u32 end_signature = 0x06054b50;
constexpr size_t end_record_size = 22;
constexpr size_t central_record_size = 46;
constexpr size_t local_record_size = 30;
constexpr u16 method_stored = 0;
constexpr u16 method_deflated = 8;
// The "made by" field's system: Unix, whose mode is then in the external attributes' top half.
constexpr u16 made_by_unix = 3 << 8;
constexpr u32 unix_directory = 0040000;
constexpr u32 unix_symbolic_link = 0120000;
constexpr u32 unix_file = 0100000;
constexpr u32 unix_type_mask = 0170000;

u16 read_u16(ReadonlyBytes bytes, size_t at)
{
    return static_cast<u16>(bytes[at] | (bytes[at + 1] << 8));
}

u32 read_u32(ReadonlyBytes bytes, size_t at)
{
    return static_cast<u32>(bytes[at]) | (static_cast<u32>(bytes[at + 1]) << 8) | (static_cast<u32>(bytes[at + 2]) << 16) | (static_cast<u32>(bytes[at + 3]) << 24);
}

void write_u16(Vector<u8>& out, u16 value)
{
    out.append(static_cast<u8>(value));
    out.append(static_cast<u8>(value >> 8));
}

void write_u32(Vector<u8>& out, u32 value)
{
    write_u16(out, static_cast<u16>(value));
    write_u16(out, static_cast<u16>(value >> 16));
}

void write_bytes(Vector<u8>& out, ReadonlyBytes bytes)
{
    for (size_t i = 0; i < bytes.size(); ++i)
        out.append(bytes[i]);
}

u32 const* crc_table()
{
    static u32 table[256];
    static bool made = false;
    if (!made) {
        for (u32 i = 0; i < 256; ++i) {
            u32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? 0xedb88320u ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        made = true;
    }
    return table;
}

u32 crc32_of(ReadonlyBytes bytes)
{
    auto const* table = crc_table();
    u32 value = 0xffffffffu;
    for (size_t i = 0; i < bytes.size(); ++i)
        value = table[(value ^ bytes[i]) & 0xff] ^ (value >> 8);
    return value ^ 0xffffffffu;
}

}

ErrorOr<ZipReader> ZipReader::parse(ReadonlyBytes archive)
{
    if (archive.size() < end_record_size)
        return Error::from_string_literal("too short to be a zip archive");
    // Looking back from the end: a comment may follow the record.
    Optional<size_t> end_at;
    size_t const earliest = archive.size() > end_record_size + 65535 ? archive.size() - end_record_size - 65535 : 0;
    for (size_t at = archive.size() - end_record_size + 1; at > earliest;) {
        --at;
        if (read_u32(archive, at) == end_signature) {
            end_at = at;
            break;
        }
    }
    if (!end_at.has_value())
        return Error::from_string_literal("not a zip archive");
    u16 const count = read_u16(archive, end_at.value() + 10);
    u32 const directory_at = read_u32(archive, end_at.value() + 16);

    ZipReader reader;
    reader.m_archive = archive;
    size_t at = directory_at;
    for (u16 i = 0; i < count; ++i) {
        if (at + central_record_size > archive.size() || read_u32(archive, at) != central_signature)
            return Error::from_string_literal("a zip archive's directory is damaged");
        u16 const made_by = read_u16(archive, at + 4);
        u16 const flags = read_u16(archive, at + 8);
        if (flags & 1)
            return Error::from_string_literal("encrypted zip archives aren't supported");
        ZipEntry entry;
        entry.method = read_u16(archive, at + 10);
        entry.dos_time = read_u16(archive, at + 12);
        entry.dos_date = read_u16(archive, at + 14);
        entry.crc32 = read_u32(archive, at + 16);
        entry.compressed_size = read_u32(archive, at + 20);
        entry.size = read_u32(archive, at + 24);
        size_t const name_length = read_u16(archive, at + 28);
        size_t const extra_length = read_u16(archive, at + 30);
        size_t const comment_length = read_u16(archive, at + 32);
        u32 const external = read_u32(archive, at + 38);
        entry.local_header_offset = read_u32(archive, at + 42);
        size_t const next = at + central_record_size + name_length + extra_length + comment_length;
        if (next > archive.size())
            return Error::from_string_literal("a zip archive's directory is damaged");
        auto name = StringView(archive.slice(at + central_record_size, name_length));
        if ((made_by >> 8) == 3) {
            u32 const unix_mode = external >> 16;
            entry.mode = unix_mode & 07777;
            entry.is_directory = (unix_mode & unix_type_mask) == unix_directory;
            entry.is_symbolic_link = (unix_mode & unix_type_mask) == unix_symbolic_link;
        }
        if (name.ends_with('/')) {
            entry.is_directory = true;
            name = name.substring_view(0, name.length() - 1);
        }
        if (entry.mode == 0)
            entry.mode = entry.is_directory ? 0755 : 0644;
        entry.path = TRY(String::from_view(name));
        TRY(reader.m_entries.try_append(move(entry)));
        at = next;
    }
    return reader;
}

ErrorOr<Vector<u8>> ZipReader::read(ZipEntry const& entry) const
{
    size_t const at = entry.local_header_offset;
    if (at + local_record_size > m_archive.size() || read_u32(m_archive, at) != local_signature)
        return Error::from_string_literal("a zip archive's member is damaged");
    size_t const name_length = read_u16(m_archive, at + 26);
    size_t const extra_length = read_u16(m_archive, at + 28);
    size_t const data_at = at + local_record_size + name_length + extra_length;
    if (data_at + entry.compressed_size > m_archive.size())
        return Error::from_string_literal("a zip archive's member is cut short");
    auto compressed = m_archive.slice(data_at, entry.compressed_size);
    Vector<u8> data;
    if (entry.method == method_stored) {
        TRY(data.try_resize(compressed.size()));
        if (!compressed.is_empty())
            __builtin_memcpy(data.data(), compressed.data(), compressed.size());
    } else if (entry.method == method_deflated) {
        data = TRY(Compress::inflate(compressed, entry.size));
    } else {
        return Error::from_string_literal("a zip archive's member is compressed in a way that isn't supported");
    }
    if (data.size() != entry.size || crc32_of(data.span()) != entry.crc32)
        return Error::from_string_literal("a zip archive's member fails its checksum");
    return data;
}

ErrorOr<void> ZipWriter::add(StringView path, u32 unix_mode, ReadonlyBytes data, bool may_compress, u16 dos_date, u16 dos_time)
{
    u32 const crc = crc32_of(data);
    u16 method = method_stored;
    Vector<u8> compressed;
    ReadonlyBytes stored = data;
    if (may_compress && !data.is_empty()) {
        compressed = TRY(Compress::deflate(data));
        if (compressed.size() < data.size()) {
            method = method_deflated;
            stored = compressed.span();
        }
    }
    u32 const offset = static_cast<u32>(m_body.size());
    u16 const needed_version = method == method_deflated ? 20 : 10;
    // Bit 11: the name is UTF-8.
    u16 const flags = 1 << 11;

    write_u32(m_body, local_signature);
    write_u16(m_body, needed_version);
    write_u16(m_body, flags);
    write_u16(m_body, method);
    write_u16(m_body, dos_time);
    write_u16(m_body, dos_date);
    write_u32(m_body, crc);
    write_u32(m_body, static_cast<u32>(stored.size()));
    write_u32(m_body, static_cast<u32>(data.size()));
    write_u16(m_body, static_cast<u16>(path.length()));
    write_u16(m_body, 0);
    write_bytes(m_body, path.bytes());
    write_bytes(m_body, stored);

    write_u32(m_directory, central_signature);
    write_u16(m_directory, static_cast<u16>(made_by_unix | 20));
    write_u16(m_directory, needed_version);
    write_u16(m_directory, flags);
    write_u16(m_directory, method);
    write_u16(m_directory, dos_time);
    write_u16(m_directory, dos_date);
    write_u32(m_directory, crc);
    write_u32(m_directory, static_cast<u32>(stored.size()));
    write_u32(m_directory, static_cast<u32>(data.size()));
    write_u16(m_directory, static_cast<u16>(path.length()));
    write_u16(m_directory, 0);
    write_u16(m_directory, 0);
    write_u16(m_directory, 0);
    write_u16(m_directory, 0);
    write_u32(m_directory, unix_mode << 16);
    write_u32(m_directory, offset);
    write_bytes(m_directory, path.bytes());
    ++m_count;
    return {};
}

ErrorOr<void> ZipWriter::add_directory(StringView path, u32 mode, u16 dos_date, u16 dos_time)
{
    StringBuilder name;
    name.append(path);
    if (!path.ends_with('/'))
        name.append('/');
    return add(name.view(), unix_directory | (mode & 07777), {}, false, dos_date, dos_time);
}

ErrorOr<void> ZipWriter::add_file(StringView path, u32 mode, ReadonlyBytes data, u16 dos_date, u16 dos_time)
{
    return add(path, unix_file | (mode & 07777), data, true, dos_date, dos_time);
}

ErrorOr<void> ZipWriter::add_symbolic_link(StringView path, StringView target, u16 dos_date, u16 dos_time)
{
    return add(path, unix_symbolic_link | 0755, target.bytes(), false, dos_date, dos_time);
}

ErrorOr<Vector<u8>> ZipWriter::finish()
{
    Vector<u8> archive;
    TRY(archive.try_resize(0));
    write_bytes(archive, m_body.span());
    u32 const directory_at = static_cast<u32>(archive.size());
    write_bytes(archive, m_directory.span());
    write_u32(archive, end_signature);
    write_u16(archive, 0);
    write_u16(archive, 0);
    write_u16(archive, m_count);
    write_u16(archive, m_count);
    write_u32(archive, static_cast<u32>(m_directory.size()));
    write_u32(archive, directory_at);
    write_u16(archive, 0);
    return archive;
}

}
