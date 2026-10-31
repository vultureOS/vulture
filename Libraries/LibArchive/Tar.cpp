/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Tar.h>
#include <LibBase/Optional.h>

namespace Archive {

static constexpr size_t block_size = 512;

// The ustar header (POSIX.1-1988), one 512-byte block.
struct Header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

static_assert(sizeof(Header) == block_size);

static StringView field(char const* characters, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && characters[length] != '\0')
        ++length;
    return { characters, length };
}

static Optional<u64> parse_octal(char const* characters, size_t capacity)
{
    u64 value = 0;
    size_t index = 0;
    while (index < capacity && characters[index] == ' ')
        ++index;
    bool any = false;
    for (; index < capacity && characters[index] >= '0' && characters[index] <= '7'; ++index) {
        if (value > (~0ull >> 3))
            return {};
        value = value * 8 + static_cast<u64>(characters[index] - '0');
        any = true;
    }
    if (!any)
        return {};
    return value;
}

static u32 compute_checksum(Header const& header)
{
    auto const* bytes = reinterpret_cast<u8 const*>(&header);
    u32 sum = 0;
    for (size_t i = 0; i < block_size; ++i) {
        bool in_checksum_field = i >= __builtin_offsetof(Header, checksum) && i < __builtin_offsetof(Header, checksum) + sizeof(header.checksum);
        sum += in_checksum_field ? static_cast<u8>(' ') : bytes[i];
    }
    return sum;
}

static bool is_zero_block(u8 const* block)
{
    for (size_t i = 0; i < block_size; ++i) {
        if (block[i] != 0)
            return false;
    }
    return true;
}

StringView TarReader::normalize(StringView path)
{
    while (path.starts_with("./"_sv))
        path = path.substring_view(2);
    while (path.starts_with("/"_sv))
        path = path.substring_view(1);
    while (path.ends_with("/"_sv))
        path = path.substring_view(0, path.length() - 1);
    if (path == "."_sv)
        return ""_sv;
    return path;
}

ErrorOr<TarReader> TarReader::parse(ReadonlyBytes archive)
{
    if (archive.size() % block_size != 0)
        return Error::from_string_literal("tar: size is not a multiple of 512 bytes");
    TarReader reader;
    reader.m_archive = archive;
    // Validate every header up front, so lookups cannot fail later.
    TRY(reader.walk([](void*, TarEntry const&) { return IterationDecision::Continue; }, nullptr));
    return reader;
}

ErrorOr<void> TarReader::walk(Visitor visitor, void* context) const
{
    // Full paths ("prefix/name") are assembled here, one entry at a time.
    char path_buffer[256 + 1];
    size_t offset = 0;
    while (offset + block_size <= m_archive.size()) {
        auto const* block = m_archive.data() + offset;
        if (is_zero_block(block))
            return {};
        auto const& header = *reinterpret_cast<Header const*>(block);
        if (field(header.magic, 5) != "ustar"_sv)
            return Error::from_string_literal("tar: not a ustar header");
        auto checksum = parse_octal(header.checksum, sizeof(header.checksum));
        if (!checksum.has_value() || checksum.value() != compute_checksum(header))
            return Error::from_string_literal("tar: header checksum mismatch");
        auto size = parse_octal(header.size, sizeof(header.size));
        auto mode = parse_octal(header.mode, sizeof(header.mode));
        if (!size.has_value() || !mode.has_value())
            return Error::from_string_literal("tar: malformed header");
        u64 data_offset = offset + block_size;
        if (size.value() > m_archive.size() - data_offset)
            return Error::from_string_literal("tar: entry extends past the end");

        auto prefix = field(header.prefix, sizeof(header.prefix));
        auto name = field(header.name, sizeof(header.name));
        size_t length = 0;
        for (char c : prefix)
            path_buffer[length++] = c;
        if (!prefix.is_empty())
            path_buffer[length++] = '/';
        for (char c : name)
            path_buffer[length++] = c;

        TarEntry entry;
        entry.path = normalize({ path_buffer, length });
        entry.mode = static_cast<u32>(mode.value());
        entry.data = m_archive.slice(data_offset, size.value());
        entry.link_target = field(header.linkname, sizeof(header.linkname));
        switch (header.typeflag) {
        case '0':
        case '\0':
            entry.type = TarEntryType::File;
            break;
        case '5':
            entry.type = TarEntryType::Directory;
            break;
        case '2':
            entry.type = TarEntryType::SymbolicLink;
            break;
        default:
            entry.type = TarEntryType::Other;
            break;
        }
        if (visitor(context, entry) == IterationDecision::Break)
            return {};

        offset = data_offset + ((size.value() + block_size - 1) / block_size) * block_size;
    }
    return {};
}

Optional<TarEntry> TarReader::find(StringView path) const
{
    auto wanted = normalize(path);
    Optional<TarEntry> found;
    (void)for_each_entry([&](TarEntry const& entry) {
        if (entry.path != wanted)
            return IterationDecision::Continue;
        found = entry;
        // A visited entry's path may point into a temporary buffer.
        found->path = wanted;
        return IterationDecision::Break;
    });
    return found;
}

static void write_octal(char* destination, size_t capacity, u64 value)
{
    // capacity - 1 digits, zero-padded, then a NUL.
    destination[capacity - 1] = '\0';
    for (size_t i = capacity - 1; i-- > 0;) {
        destination[i] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
}

static void copy_field(char* destination, size_t capacity, StringView text)
{
    for (size_t i = 0; i < text.length() && i < capacity; ++i)
        destination[i] = text[i];
}

ErrorOr<void> TarWriter::write_header(StringView path, TarEntryType type, u32 mode, u64 size)
{
    Header header {};
    __builtin_memset(&header, 0, sizeof(header));
    // Long paths split at a slash into prefix (155) and name (100).
    if (path.length() <= sizeof(header.name)) {
        copy_field(header.name, sizeof(header.name), path);
    } else {
        Optional<size_t> split;
        for (size_t i = 0; i < path.length(); ++i) {
            if (path[i] == '/' && i <= sizeof(header.prefix) && path.length() - i - 1 <= sizeof(header.name))
                split = i;
        }
        if (!split.has_value())
            return Error::from_string_literal("tar: path too long");
        copy_field(header.prefix, sizeof(header.prefix), path.substring_view(0, split.value()));
        copy_field(header.name, sizeof(header.name), path.substring_view(split.value() + 1));
    }
    write_octal(header.mode, sizeof(header.mode), mode & 07777);
    write_octal(header.uid, sizeof(header.uid), 0);
    write_octal(header.gid, sizeof(header.gid), 0);
    write_octal(header.size, sizeof(header.size), size);
    write_octal(header.mtime, sizeof(header.mtime), 0);
    header.typeflag = type == TarEntryType::Directory ? '5' : '0';
    copy_field(header.magic, sizeof(header.magic), "ustar"_sv);
    header.version[0] = '0';
    header.version[1] = '0';
    copy_field(header.uname, sizeof(header.uname), "root"_sv);
    copy_field(header.gname, sizeof(header.gname), "wheel"_sv);
    // The checksum field is six octal digits, a NUL and a space.
    u32 checksum = compute_checksum(header);
    write_octal(header.checksum, 7, checksum);
    header.checksum[7] = ' ';
    m_sink.write({ reinterpret_cast<u8 const*>(&header), sizeof(header) });
    return {};
}

ErrorOr<void> TarWriter::add_directory(StringView path, u32 mode)
{
    return write_header(TarReader::normalize(path), TarEntryType::Directory, mode, 0);
}

ErrorOr<void> TarWriter::add_file(StringView path, u32 mode, ReadonlyBytes data)
{
    TRY(write_header(TarReader::normalize(path), TarEntryType::File, mode, data.size()));
    m_sink.write(data);
    static u8 const zeros[block_size] {};
    size_t padding = (block_size - data.size() % block_size) % block_size;
    if (padding)
        m_sink.write({ zeros, padding });
    return {};
}

void TarWriter::finish()
{
    static u8 const zeros[block_size * 2] {};
    m_sink.write({ zeros, sizeof(zeros) });
}

}
