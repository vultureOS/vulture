/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/FileSystem/FATFS.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/Heap.h>
#include <LibBase/CharacterTypes.h>
#include <LibBase/New.h>

namespace Kernel {

namespace {

struct [[gnu::packed]] BootSector {
    u8 jump[3];
    char oem[8];
    u16 bytes_per_sector;
    u8 sectors_per_cluster;
    u16 reserved_sectors;
    u8 fat_count;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8 media;
    u16 fat_size_16;
    u16 sectors_per_track;
    u16 heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    // FAT32 only:
    u32 fat_size_32;
    u16 flags;
    u16 version;
    u32 root_cluster;
};

struct [[gnu::packed]] DirectoryEntry {
    char name[11];
    u8 attributes;
    u8 reserved;
    u8 creation_tenths;
    u16 creation_time;
    u16 creation_date;
    u16 access_date;
    u16 cluster_high;
    u16 modification_time;
    u16 modification_date;
    u16 cluster_low;
    u32 size;
};

struct [[gnu::packed]] LongNameEntry {
    u8 order;
    u16 name1[5];
    u8 attributes;
    u8 type;
    u8 checksum;
    u16 name2[6];
    u16 zero;
    u16 name3[2];
};

static_assert(sizeof(DirectoryEntry) == 32 && sizeof(LongNameEntry) == 32);

constexpr u8 attribute_read_only = 0x01;
constexpr u8 attribute_volume = 0x08;
constexpr u8 attribute_directory = 0x10;
constexpr u8 attribute_long_name = 0x0f;

enum class Variant : u8 {
    FAT12,
    FAT16,
    FAT32,
};

// FAT dates count from 1980; seconds in steps of two.
i64 to_unix_time(u16 date, u16 time)
{
    if (date == 0)
        return 0;
    int year = 1980 + (date >> 9);
    int month = (date >> 5) & 0xf;
    int day = date & 0x1f;
    // Days from 1970-01-01 (civil-from-days, Howard Hinnant's algorithm).
    int y = year - (month <= 2);
    int era = y / 400;
    int year_of_era = y - era * 400;
    int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    i64 days = static_cast<i64>(era) * 146097 + day_of_era - 719468;
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 0x3f) * 60 + (time & 0x1f) * 2;
}

}

class FATInode;

class FATFS final : public FileSystem {
public:
    static ErrorOr<NonnullRefPtr<FATFS>> create(BlockDevice& device)
    {
        auto* file_system = new (Base::nothrow) FATFS(device);
        if (!file_system)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*file_system);
        TRY(reference->read_boot_sector());
        return reference;
    }

    ~FATFS() override { kfree(m_buffer); }

    [[nodiscard]] StringView name() const override { return "fat"_sv; }
    [[nodiscard]] NonnullRefPtr<Inode> root_inode() override;
    [[nodiscard]] bool is_read_only() const override { return true; }

    [[nodiscard]] Variant variant() const { return m_variant; }
    [[nodiscard]] size_t cluster_size() const { return m_cluster_size; }

    // Reads a whole cluster (or, cluster 0 on FAT12/16, the fixed root
    // directory region, sector by sector through `sector`) into `destination`.
    ErrorOr<void> read_cluster(u32 cluster, u8* destination)
    {
        u64 sector = m_data_start + static_cast<u64>(cluster - 2) * m_sectors_per_cluster;
        return m_device->read_blocks(sector * m_sector_blocks, m_sectors_per_cluster * m_sector_blocks, { destination, m_cluster_size });
    }

    ErrorOr<void> read_root_sector(u32 index, u8* destination)
    {
        return m_device->read_blocks((m_root_start + index) * m_sector_blocks, m_sector_blocks, { destination, m_bytes_per_sector });
    }

    [[nodiscard]] u32 root_sector_count() const { return m_root_sectors; }
    [[nodiscard]] u32 root_cluster() const { return m_root_cluster; }
    [[nodiscard]] u32 bytes_per_sector() const { return m_bytes_per_sector; }

    // The next cluster in a chain, or nothing at its end.
    ErrorOr<Optional<u32>> next_cluster(u32 cluster)
    {
        u64 offset;
        switch (m_variant) {
        case Variant::FAT12:
            offset = cluster + cluster / 2;
            break;
        case Variant::FAT16:
            offset = cluster * 2ull;
            break;
        case Variant::FAT32:
            offset = cluster * 4ull;
            break;
        }
        u64 sector = m_fat_start + offset / m_bytes_per_sector;
        u32 within = static_cast<u32>(offset % m_bytes_per_sector);
        // Two sectors: a FAT12 entry may straddle a sector boundary.
        TRY(read_fat_sectors(sector));
        u32 value;
        switch (m_variant) {
        case Variant::FAT12: {
            u16 pair = static_cast<u16>(m_buffer[within] | (m_buffer[within + 1] << 8));
            value = (cluster & 1) ? (pair >> 4) : (pair & 0xfff);
            if (value >= 0xff8)
                return Optional<u32> {};
            break;
        }
        case Variant::FAT16:
            value = static_cast<u32>(m_buffer[within] | (m_buffer[within + 1] << 8));
            if (value >= 0xfff8)
                return Optional<u32> {};
            break;
        case Variant::FAT32:
            value = (static_cast<u32>(m_buffer[within]) | (static_cast<u32>(m_buffer[within + 1]) << 8) | (static_cast<u32>(m_buffer[within + 2]) << 16) | (static_cast<u32>(m_buffer[within + 3]) << 24)) & 0x0fff'ffff;
            if (value >= 0x0fff'fff8)
                return Optional<u32> {};
            break;
        }
        if (value < 2 || value >= m_cluster_count + 2)
            return Error::from_errno(EIO);
        return Optional<u32>(value);
    }

    [[nodiscard]] Mutex& lock() { return m_lock; }
    [[nodiscard]] InodeIndex index_for(u32 cluster, u64 entry_position) const { return cluster ? cluster : entry_position + (1ull << 40); }

private:
    explicit FATFS(BlockDevice& device)
        : m_device(device)
    {
    }

    ErrorOr<void> read_fat_sectors(u64 sector)
    {
        if (m_cached_fat_sector == sector)
            return {};
        TRY(m_device->read_blocks(sector * m_sector_blocks, 2 * m_sector_blocks, { m_buffer, 2ull * m_bytes_per_sector }));
        m_cached_fat_sector = sector;
        return {};
    }

    ErrorOr<void> read_boot_sector();

    NonnullRefPtr<BlockDevice> m_device;
    Mutex m_lock;
    Variant m_variant { Variant::FAT32 };
    u32 m_bytes_per_sector { 512 };
    u32 m_sector_blocks { 1 }; // Device blocks per FAT sector.
    u32 m_sectors_per_cluster { 1 };
    size_t m_cluster_size { 512 };
    u64 m_fat_start { 0 };
    u64 m_root_start { 0 };
    u32 m_root_sectors { 0 };
    u64 m_data_start { 0 };
    u32 m_cluster_count { 0 };
    u32 m_root_cluster { 0 };
    u8* m_buffer { nullptr };
    u64 m_cached_fat_sector { ~0ull };
    RefPtr<FATInode> m_root;
};

class FATInode final : public Inode {
public:
    FATInode(FATFS& file_system, InodeIndex index, u32 first_cluster, u32 size, u8 attributes, i64 modification_time, bool is_root)
        : Inode(file_system, index)
        , m_first_cluster(first_cluster)
        , m_size(size)
        , m_attributes(attributes)
        , m_modification_time(modification_time)
        , m_is_root(is_root)
    {
    }

    [[nodiscard]] InodeMetadata metadata() const override
    {
        InodeMetadata metadata;
        metadata.index = index();
        bool directory = m_is_root || (m_attributes & attribute_directory);
        metadata.type = directory ? InodeType::Directory : InodeType::File;
        metadata.permissions = directory ? 0755 : ((m_attributes & attribute_read_only) ? 0444 : 0644);
        metadata.link_count = directory ? 2 : 1;
        metadata.size = directory ? 0 : m_size;
        metadata.modification_time = m_modification_time;
        metadata.block_size = static_cast<u32>(fat().cluster_size());
        return metadata;
    }

    ErrorOr<size_t> read(u64 offset, size_t count, IOBuffer const& buffer) override
    {
        if (metadata().type == InodeType::Directory)
            return Error::from_errno(EISDIR);
        if (offset >= m_size)
            return 0;
        count = min<u64>(count, m_size - offset);
        size_t cluster_size = fat().cluster_size();
        auto* data = static_cast<u8*>(kmalloc(cluster_size));
        if (!data)
            return Error::from_errno(ENOMEM);
        size_t done = 0;
        ErrorOr<void> result {};
        {
            MutexLocker locker(fat().lock());
            u32 cluster = m_first_cluster;
            // Walk to the cluster holding `offset`.
            for (u64 skip = offset / cluster_size; skip > 0 && !result.is_error(); --skip) {
                auto next = fat().next_cluster(cluster);
                if (next.is_error() || !next.value().has_value()) {
                    result = Error::from_errno(EIO);
                    break;
                }
                cluster = next.value().value();
            }
            size_t within = offset % cluster_size;
            while (done < count && !result.is_error()) {
                result = fat().read_cluster(cluster, data);
                if (result.is_error())
                    break;
                size_t length = min(cluster_size - within, count - done);
                result = buffer.write(done, { data + within, length });
                done += length;
                within = 0;
                if (done < count) {
                    auto next = fat().next_cluster(cluster);
                    if (next.is_error() || !next.value().has_value()) {
                        result = Error::from_errno(EIO);
                        break;
                    }
                    cluster = next.value().value();
                }
            }
        }
        kfree(data);
        TRY(result);
        return done;
    }

    ErrorOr<NonnullRefPtr<Inode>> lookup(StringView name) override
    {
        if (name == "."_sv)
            return NonnullRefPtr<Inode>(*this);
        RefPtr<Inode> found;
        TRY(for_each_entry([&](StringView entry_name, DirectoryEntry const& entry, u64 position) {
            if (!names_match(entry_name, name))
                return VisitDecision::Continue;
            found = make_child(entry, position);
            return VisitDecision::Stop;
        }));
        if (!found)
            return Error::from_errno(ENOENT);
        return found.release_nonnull();
    }

    ErrorOr<void> traverse(DirectoryVisitor& visitor) override
    {
        if (metadata().type != InodeType::Directory)
            return Error::from_errno(ENOTDIR);
        if (m_is_root) {
            if (visitor.visit("."_sv, index(), InodeType::Directory) == VisitDecision::Stop || visitor.visit(".."_sv, index(), InodeType::Directory) == VisitDecision::Stop)
                return {};
        }
        return for_each_entry([&](StringView name, DirectoryEntry const& entry, u64 position) {
            u32 cluster = (static_cast<u32>(entry.cluster_high) << 16) | entry.cluster_low;
            auto type = (entry.attributes & attribute_directory) ? InodeType::Directory : InodeType::File;
            return visitor.visit(name, fat().index_for(cluster, position), type);
        });
    }

private:
    FATFS& fat() const { return static_cast<FATFS&>(file_system()); }

    static bool names_match(StringView a, StringView b)
    {
        // FAT names are case-insensitive.
        if (a.length() != b.length())
            return false;
        for (size_t i = 0; i < a.length(); ++i) {
            if (Base::to_ascii_lowercase(a[i]) != Base::to_ascii_lowercase(b[i]))
                return false;
        }
        return true;
    }

    NonnullRefPtr<Inode> make_child(DirectoryEntry const& entry, u64 position)
    {
        u32 cluster = (static_cast<u32>(entry.cluster_high) << 16) | entry.cluster_low;
        // ".." pointing at cluster 0 means the root directory.
        if (cluster == 0 && (entry.attributes & attribute_directory))
            return fat().root_inode();
        return adopt_ref(*new FATInode(fat(), fat().index_for(cluster, position), cluster, entry.size, entry.attributes,
            to_unix_time(entry.modification_date, entry.modification_time), false));
    }

    // Calls `callback(name, entry, position)` for every entry, with long
    // names assembled (ASCII; other characters become '?').
    template<typename Callback>
    ErrorOr<void> for_each_entry(Callback callback)
    {
        if (metadata().type != InodeType::Directory)
            return Error::from_errno(ENOTDIR);
        MutexLocker locker(fat().lock());
        bool fixed_root = m_is_root && fat().variant() != Variant::FAT32;
        size_t unit = fixed_root ? fat().bytes_per_sector() : fat().cluster_size();
        auto* data = static_cast<u8*>(kmalloc(unit));
        if (!data)
            return Error::from_errno(ENOMEM);
        char long_name[256];
        size_t long_length = 0;
        u32 cluster = m_is_root && !fixed_root ? fat().root_cluster() : m_first_cluster;
        ErrorOr<void> result {};
        bool stop = false;
        for (u32 unit_index = 0; !stop && !result.is_error(); ++unit_index) {
            if (fixed_root) {
                if (unit_index >= fat().root_sector_count())
                    break;
                result = fat().read_root_sector(unit_index, data);
            } else {
                if (unit_index > 0) {
                    auto next = fat().next_cluster(cluster);
                    if (next.is_error()) {
                        result = next.release_error();
                        break;
                    }
                    if (!next.value().has_value())
                        break;
                    cluster = next.value().value();
                }
                result = fat().read_cluster(cluster, data);
            }
            if (result.is_error())
                break;
            for (size_t offset = 0; offset < unit && !stop; offset += sizeof(DirectoryEntry)) {
                auto const& entry = *reinterpret_cast<DirectoryEntry const*>(data + offset);
                if (entry.name[0] == 0) {
                    stop = true;
                    break;
                }
                if (static_cast<u8>(entry.name[0]) == 0xe5) {
                    long_length = 0;
                    continue;
                }
                if (entry.attributes == attribute_long_name) {
                    auto const& part = *reinterpret_cast<LongNameEntry const*>(&entry);
                    size_t order = (part.order & 0x1f);
                    if (order == 0 || order > 20)
                        continue;
                    u16 characters[13];
                    __builtin_memcpy(characters, part.name1, sizeof(part.name1));
                    __builtin_memcpy(characters + 5, part.name2, sizeof(part.name2));
                    __builtin_memcpy(characters + 11, part.name3, sizeof(part.name3));
                    size_t base = (order - 1) * 13;
                    for (size_t i = 0; i < 13; ++i) {
                        if (characters[i] == 0x0000 || characters[i] == 0xffff)
                            break;
                        if (base + i < sizeof(long_name) - 1) {
                            long_name[base + i] = characters[i] < 0x80 ? static_cast<char>(characters[i]) : '?';
                            long_length = max(long_length, base + i + 1);
                        }
                    }
                    continue;
                }
                if (entry.attributes & attribute_volume) {
                    long_length = 0;
                    continue;
                }
                char short_name[13];
                size_t short_length = 0;
                StringView name;
                if (long_length > 0) {
                    name = { long_name, long_length };
                } else {
                    // "NAME    EXT" becomes "name.ext".
                    for (size_t i = 0; i < 8 && entry.name[i] != ' '; ++i)
                        short_name[short_length++] = Base::to_ascii_lowercase(entry.name[i]);
                    if (entry.name[8] != ' ') {
                        short_name[short_length++] = '.';
                        for (size_t i = 8; i < 11 && entry.name[i] != ' '; ++i)
                            short_name[short_length++] = Base::to_ascii_lowercase(entry.name[i]);
                    }
                    name = { short_name, short_length };
                }
                long_length = 0;
                u64 position = static_cast<u64>(cluster) * 65536 + unit_index * unit + offset;
                if (callback(name, entry, position) == VisitDecision::Stop)
                    stop = true;
            }
        }
        kfree(data);
        return result;
    }

    u32 m_first_cluster;
    u32 m_size;
    u8 m_attributes;
    i64 m_modification_time;
    bool m_is_root;
};

ErrorOr<void> FATFS::read_boot_sector()
{
    auto* sector = static_cast<u8*>(kmalloc(BlockDevice::block_size));
    if (!sector)
        return Error::from_errno(ENOMEM);
    auto read = m_device->read_blocks(0, 1, { sector, BlockDevice::block_size });
    BootSector boot;
    __builtin_memcpy(&boot, sector, sizeof(boot));
    bool signed_sector = sector[510] == 0x55 && sector[511] == 0xaa;
    kfree(sector);
    TRY(read);
    if (!signed_sector || boot.bytes_per_sector < 512 || boot.bytes_per_sector % 512 || boot.sectors_per_cluster == 0 || boot.fat_count == 0)
        return Error::from_string_literal("not a FAT filesystem");

    m_bytes_per_sector = boot.bytes_per_sector;
    m_sector_blocks = m_bytes_per_sector / BlockDevice::block_size;
    m_sectors_per_cluster = boot.sectors_per_cluster;
    m_cluster_size = static_cast<size_t>(m_bytes_per_sector) * m_sectors_per_cluster;
    u32 fat_size = boot.fat_size_16 ? boot.fat_size_16 : boot.fat_size_32;
    u32 total = boot.total_sectors_16 ? boot.total_sectors_16 : boot.total_sectors_32;
    m_root_sectors = (boot.root_entry_count * 32u + m_bytes_per_sector - 1) / m_bytes_per_sector;
    m_fat_start = boot.reserved_sectors;
    m_root_start = m_fat_start + static_cast<u64>(boot.fat_count) * fat_size;
    m_data_start = m_root_start + m_root_sectors;
    if (total <= m_data_start)
        return Error::from_string_literal("FAT: inconsistent geometry");
    m_cluster_count = static_cast<u32>((total - m_data_start) / m_sectors_per_cluster);
    m_variant = m_cluster_count < 4085 ? Variant::FAT12 : (m_cluster_count < 65525 ? Variant::FAT16 : Variant::FAT32);
    m_root_cluster = m_variant == Variant::FAT32 ? boot.root_cluster : 0;

    m_buffer = static_cast<u8*>(kmalloc(2ull * m_bytes_per_sector));
    if (!m_buffer)
        return Error::from_errno(ENOMEM);
    auto* root = new (Base::nothrow) FATInode(*this, 1, m_root_cluster, 0, attribute_directory, 0, true);
    if (!root)
        return Error::from_errno(ENOMEM);
    m_root = adopt_ref(*root);
    return {};
}

NonnullRefPtr<Inode> FATFS::root_inode()
{
    return *m_root;
}

ErrorOr<NonnullRefPtr<FileSystem>> create_fat_file_system(BlockDevice& device)
{
    auto file_system = TRY(FATFS::create(device));
    return NonnullRefPtr<FileSystem>(*file_system);
}

}
