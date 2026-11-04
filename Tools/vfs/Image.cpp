/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Image.h"
#include <Kernel/API/POSIX/errno.h>
#include <LibBase/Checksum.h>

extern "C" {
struct HostTimespec {
    long seconds;
    long nanoseconds;
};
int clock_gettime(int clock, HostTimespec*);
}

namespace Tools {

static constexpr int seek_set = 0;
static constexpr int seek_end = 2;
static constexpr u32 gpt_entry_count = 128;
static constexpr u32 gpt_entry_size = 128;
static constexpr u64 gpt_entry_sectors = gpt_entry_count * gpt_entry_size / sector_size;

namespace {

struct [[gnu::packed]] MBREntry {
    u8 status;
    u8 chs_first[3];
    u8 type;
    u8 chs_last[3];
    u32 first_lba;
    u32 sector_count;
};

struct [[gnu::packed]] GPTHeader {
    char signature[8];
    u32 revision;
    u32 header_size;
    u32 header_crc;
    u32 reserved;
    u64 my_lba;
    u64 alternate_lba;
    u64 first_usable_lba;
    u64 last_usable_lba;
    u8 disk_guid[16];
    u64 entries_lba;
    u32 entry_count;
    u32 entry_size;
    u32 entries_crc;
};
static_assert(sizeof(GPTHeader) == 92);

struct [[gnu::packed]] GPTEntry {
    u8 type[16];
    u8 unique[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u16 name[36];
};
static_assert(sizeof(GPTEntry) == gpt_entry_size);

}

ErrorOr<Vector<u8>> read_file(char const* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return Error::from_errno(ENOENT);
    fseek(file, 0, seek_end);
    long size = ftell(file);
    fseek(file, 0, seek_set);
    Vector<u8> data;
    if (size < 0 || data.try_resize(static_cast<size_t>(size)).is_error()) {
        fclose(file);
        return Error::from_errno(ENOMEM);
    }
    bool ok = size == 0 || fread(data.begin(), 1, data.size(), file) == data.size();
    fclose(file);
    if (!ok)
        return Error::from_errno(EIO);
    return data;
}

void random_bytes(Bytes bytes)
{
    FILE* source = fopen("/dev/urandom", "rb");
    if (source && fread(bytes.data(), 1, bytes.size(), source) == bytes.size()) {
        fclose(source);
        return;
    }
    if (source)
        fclose(source);
    // No /dev/urandom: something unique enough from the clock.
    u64 state = static_cast<u64>(host_clock());
    for (auto& byte : bytes) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        byte = static_cast<u8>(state >> 56);
    }
}

i64 host_clock()
{
    HostTimespec now {};
    clock_gettime(0, &now);
    return static_cast<i64>(now.seconds) * 1'000'000'000 + now.nanoseconds;
}

ErrorOr<void> ImageStore::read_blocks(VultureFS::BlockIndex first, size_t count, Bytes buffer)
{
    if (first + count > m_blocks || buffer.size() < count * VultureFS::block_size)
        return Error::from_errno(EINVAL);
    if (fseek(m_file, static_cast<long>(m_offset + first * VultureFS::block_size), seek_set) != 0)
        return Error::from_errno(EIO);
    size_t bytes = count * VultureFS::block_size;
    size_t got = fread(buffer.data(), 1, bytes, m_file);
    // Past the end of a sparse file that was never written: zeros.
    for (size_t i = got; i < bytes; ++i)
        buffer[i] = 0;
    return {};
}

ErrorOr<void> ImageStore::write_blocks(VultureFS::BlockIndex first, size_t count, ReadonlyBytes buffer)
{
    if (first + count > m_blocks || buffer.size() < count * VultureFS::block_size)
        return Error::from_errno(EINVAL);
    if (fseek(m_file, static_cast<long>(m_offset + first * VultureFS::block_size), seek_set) != 0)
        return Error::from_errno(EIO);
    if (fwrite(buffer.data(), 1, count * VultureFS::block_size, m_file) != count * VultureFS::block_size)
        return Error::from_errno(EIO);
    return {};
}

ErrorOr<void> ImageStore::flush()
{
    if (fflush(m_file) != 0 || fsync(fileno(m_file)) != 0)
        return Error::from_errno(EIO);
    return {};
}

static ErrorOr<void> write_at(FILE* file, u64 offset, void const* data, size_t size)
{
    if (fseek(file, static_cast<long>(offset), seek_set) != 0 || fwrite(data, 1, size, file) != size)
        return Error::from_errno(EIO);
    return {};
}

u8 const efi_system_partition_type[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };

u64 last_usable_sector(u64 size)
{
    return size / sector_size - 1 - gpt_entry_sectors - 1;
}

ErrorOr<void> write_partition_table(FILE* file, u64 size, Span<PartitionSpec const> partitions)
{
    u64 sectors = size / sector_size;
    if (sectors < partition_start_sector + 2 * gpt_entry_sectors + 1024)
        return Error::from_string_literal("disk image too small");
    // A sparse file: only what is written takes space.
    u8 last = 0;
    TRY(write_at(file, size - 1, &last, 1));
    u64 last_lba = sectors - 1;

    // Protective MBR: one partition of type EE over the whole disk.
    u8 mbr[sector_size] {};
    MBREntry protective { 0, { 0, 2, 0 }, 0xee, { 0xff, 0xff, 0xff }, 1, static_cast<u32>(min<u64>(last_lba, 0xffff'ffff)) };
    __builtin_memcpy(mbr + 446, &protective, sizeof(protective));
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    TRY(write_at(file, 0, mbr, sizeof(mbr)));

    GPTEntry entries[gpt_entry_count] {};
    if (partitions.size() > gpt_entry_count)
        return Error::from_string_literal("too many partitions");
    for (size_t i = 0; i < partitions.size(); ++i) {
        auto const& partition = partitions[i];
        if (partition.first_sector < 2 + gpt_entry_sectors || partition.first_sector + partition.sector_count - 1 > last_usable_sector(size))
            return Error::from_string_literal("partition outside the disk");
        __builtin_memcpy(entries[i].type, partition.type, 16);
        random_bytes(entries[i].unique);
        entries[i].first_lba = partition.first_sector;
        entries[i].last_lba = partition.first_sector + partition.sector_count - 1;
        for (size_t c = 0; c < partition.name.length() && c < 35; ++c)
            entries[i].name[c] = static_cast<u8>(partition.name[c]);
    }
    u32 entries_crc = crc32({ reinterpret_cast<u8 const*>(entries), sizeof(entries) });

    GPTHeader header {};
    __builtin_memcpy(header.signature, "EFI PART", 8);
    header.revision = 0x0001'0000;
    header.header_size = sizeof(GPTHeader);
    header.first_usable_lba = 2 + gpt_entry_sectors;
    header.last_usable_lba = last_usable_sector(size);
    random_bytes(header.disk_guid);
    header.entry_count = gpt_entry_count;
    header.entry_size = gpt_entry_size;
    header.entries_crc = entries_crc;

    // The primary at the start, the backup at the end, each naming the
    // other.
    auto write_header = [&](u64 my_lba, u64 alternate_lba, u64 entries_lba) -> ErrorOr<void> {
        header.my_lba = my_lba;
        header.alternate_lba = alternate_lba;
        header.entries_lba = entries_lba;
        header.header_crc = 0;
        header.header_crc = crc32({ reinterpret_cast<u8 const*>(&header), sizeof(header) });
        u8 sector[sector_size] {};
        __builtin_memcpy(sector, &header, sizeof(header));
        TRY(write_at(file, my_lba * sector_size, sector, sizeof(sector)));
        TRY(write_at(file, entries_lba * sector_size, entries, sizeof(entries)));
        return {};
    };
    TRY(write_header(1, last_lba, 2));
    TRY(write_header(last_lba, 1, last_lba - gpt_entry_sectors));
    return {};
}

ErrorOr<void> add_partition_to_mbr(FILE* file, u64 first_sector, u64 sector_count)
{
    constexpr u8 fat32_with_lba = 0x0c;
    constexpr size_t first_entry = 446;
    // The protective entry stops where this starts, as tables like this usually do.
    MBREntry protective { 0, { 0, 2, 0 }, 0xee, { 0xff, 0xff, 0xff }, 1, static_cast<u32>(first_sector - 1) };
    MBREntry fat32 { 0, { 0xff, 0xff, 0xff }, fat32_with_lba, { 0xff, 0xff, 0xff }, static_cast<u32>(first_sector), static_cast<u32>(sector_count) };
    TRY(write_at(file, first_entry, &protective, sizeof(protective)));
    TRY(write_at(file, first_entry + sizeof(MBREntry), &fat32, sizeof(fat32)));
    return {};
}

ErrorOr<Vector<Partition>> find_partitions(FILE* file, u8 const* type)
{
    u8 sectors[2 * sector_size];
    if (fseek(file, 0, seek_set) != 0 || fread(sectors, 1, sizeof(sectors), file) != sizeof(sectors))
        return Error::from_errno(EIO);
    if (__builtin_memcmp(sectors + sector_size, "EFI PART", 8) != 0)
        return Error::from_string_literal("there's no partition table");

    GPTHeader header;
    __builtin_memcpy(&header, sectors + sector_size, sizeof(header));
    if (header.entry_size < sizeof(GPTEntry) || header.entry_count > 1024)
        return Error::from_string_literal("bad GPT header");
    Vector<u8> table;
    TRY(table.try_resize(static_cast<size_t>(header.entry_count) * header.entry_size));
    if (fseek(file, static_cast<long>(header.entries_lba * sector_size), seek_set) != 0 || fread(table.begin(), 1, table.size(), file) != table.size())
        return Error::from_errno(EIO);
    Vector<Partition> found;
    for (u32 i = 0; i < header.entry_count; ++i) {
        GPTEntry entry;
        __builtin_memcpy(&entry, table.begin() + static_cast<size_t>(i) * header.entry_size, sizeof(entry));
        if (__builtin_memcmp(entry.type, type, 16) == 0 && entry.last_lba >= entry.first_lba)
            TRY(found.try_append(Partition { entry.first_lba, entry.last_lba - entry.first_lba + 1 }));
    }
    return found;
}

}
