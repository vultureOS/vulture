/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/Array.h>
#include <LibBase/HashMap.h>
#include <LibBase/New.h>
#include <LibForeignFS/SectorCache.h>
#include <LibForeignFS/Volume.h>
#include <LibUnicode/Unicode.h>

// FAT12, FAT16 and FAT32, with long names. See Microsoft's "FAT: General Overview of On-Disk Format".

namespace ForeignFS {

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
};
static_assert(sizeof(BootSector) == 36);

struct [[gnu::packed]] Extended16 {
    u8 drive;
    u8 reserved;
    u8 signature;
    u32 serial;
    char label[11];
    char type[8];
};

struct [[gnu::packed]] Extended32 {
    u32 fat_size_32;
    u16 flags;
    u16 version;
    u32 root_cluster;
    u16 info_sector;
    u16 backup_boot_sector;
    u8 reserved[12];
    Extended16 rest;
};

struct [[gnu::packed]] DirectoryEntry {
    u8 name[11];
    u8 attributes;
    u8 case_flags;
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

struct [[gnu::packed]] LongEntry {
    u8 order;
    u16 name1[5];
    u8 attributes;
    u8 type;
    u8 checksum;
    u16 name2[6];
    u16 zero;
    u16 name3[2];
};
static_assert(sizeof(DirectoryEntry) == 32 && sizeof(LongEntry) == 32);

constexpr size_t entry_size = sizeof(DirectoryEntry);
constexpr u8 attribute_read_only = 0x01;
constexpr u8 attribute_hidden = 0x02;
constexpr u8 attribute_volume = 0x08;
constexpr u8 attribute_directory = 0x10;
constexpr u8 attribute_archive = 0x20;
constexpr u8 attribute_long = 0x0f;
constexpr u8 entry_free = 0xe5;
constexpr u8 long_last = 0x40;
constexpr u8 case_lower_base = 0x08;
constexpr u8 case_lower_extension = 0x10;
constexpr u32 most_file_size = 0xffffffff;

constexpr u32 fat32_info_lead = 0x41615252;
constexpr u32 fat32_info_struct = 0x61417272;
constexpr u32 fat32_info_trail = 0xaa550000;

enum class Bits : u8 {
    FAT12,
    FAT16,
    FAT32,
};

[[nodiscard]] u32 end_of_chain(Bits bits)
{
    return bits == Bits::FAT12 ? 0xfff : bits == Bits::FAT16 ? 0xffff : 0x0fffffff;
}

[[nodiscard]] bool is_end_of_chain(Bits bits, u32 value)
{
    return value >= (bits == Bits::FAT12 ? 0xff8u : bits == Bits::FAT16 ? 0xfff8u : 0x0ffffff8u);
}

[[nodiscard]] bool is_bad_cluster(Bits bits, u32 value)
{
    return value == (bits == Bits::FAT12 ? 0xff7u : bits == Bits::FAT16 ? 0xfff7u : 0x0ffffff7u);
}

struct Civil {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

// Howard Hinnant's method.
i64 days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    i64 const era = (year >= 0 ? year : year - 399) / 400;
    i64 const year_of_era = year - era * 400;
    i64 const day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    i64 const day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

Civil civil_from_seconds(i64 seconds)
{
    i64 days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    i64 const rest = seconds - days * 86400;
    days += 719468;
    i64 const era = (days >= 0 ? days : days - 146096) / 146097;
    i64 const day_of_era = days - era * 146097;
    i64 const year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    i64 const day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    i64 const shifted_month = (5 * day_of_year + 2) / 153;
    int const month = static_cast<int>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
    return {
        static_cast<int>(year_of_era + era * 400 + (month <= 2)),
        month,
        static_cast<int>(day_of_year - (153 * shifted_month + 2) / 5 + 1),
        static_cast<int>(rest / 3600),
        static_cast<int>(rest / 60 % 60),
        static_cast<int>(rest % 60),
    };
}

u16 dos_date_of(i64 seconds)
{
    auto const civil = civil_from_seconds(seconds);
    if (civil.year < 1980)
        return (1 << 5) | 1;
    return static_cast<u16>(((civil.year - 1980) << 9) | (civil.month << 5) | civil.day);
}

u16 dos_time_of(i64 seconds)
{
    auto const civil = civil_from_seconds(seconds);
    return static_cast<u16>((civil.hour << 11) | (civil.minute << 5) | (civil.second / 2));
}

i64 seconds_of(u16 date, u16 time)
{
    if (date == 0)
        return 0;
    i64 const days = days_from_civil(1980 + (date >> 9), max((date >> 5) & 15, 1), max(date & 31, 1));
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

u8 long_checksum(u8 const* short_name)
{
    u8 sum = 0;
    for (size_t i = 0; i < 11; ++i)
        sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

class FATVolume final : public Volume {
public:
    FATVolume(Store& store, Clock clock, bool read_only)
        : m_cache(store)
        , m_clock(clock)
        , m_read_only(read_only || store.is_read_only())
    {
    }

    ~FATVolume() override
    {
        m_nodes.for_each([](NodeIndex, Node*& node) { delete node; });
    }

    ErrorOr<void> open();

    [[nodiscard]] StringView type_name() const override { return "msdos"_sv; }
    [[nodiscard]] StringView label() const override { return m_label.view(); }
    [[nodiscard]] bool is_read_only() const override { return m_read_only; }
    [[nodiscard]] NodeIndex root() const override { return root_index; }
    [[nodiscard]] ErrorOr<Statistics> statistics() override;

    ErrorOr<NodeInfo> info(NodeIndex) override;
    ErrorOr<NodeInfo> lookup(NodeIndex directory, StringView name) override;
    ErrorOr<void> for_each_entry(NodeIndex directory, Function<Iteration(StringView, NodeInfo const&)>) override;
    ErrorOr<size_t> read(NodeIndex, u64 offset, Bytes) override;
    ErrorOr<size_t> write(NodeIndex, u64 offset, ReadonlyBytes) override;
    ErrorOr<void> truncate(NodeIndex, u64 size) override;
    ErrorOr<NodeInfo> create(NodeIndex directory, StringView name, bool is_directory) override;
    ErrorOr<void> remove(NodeIndex directory, StringView name) override;
    ErrorOr<void> rename(NodeIndex from_directory, StringView from_name, NodeIndex to_directory, StringView to_name) override;
    ErrorOr<void> set_times(NodeIndex, i64 accessed, i64 modified) override;
    ErrorOr<void> set_read_only(NodeIndex, bool) override;
    void forget(NodeIndex) override;
    ErrorOr<void> flush() override;
    ErrorOr<Extent> extent(NodeIndex) override;
    ErrorOr<void> repair(Function<void(StringView)>) override;

    ErrorOr<CheckReport> check();

private:
    static constexpr NodeIndex root_index = 1;

    struct Node {
        NodeIndex parent { 0 };
        // Of its short entry in the parent, and how many long entries come right before that.
        u64 entry_offset { 0 };
        u32 long_entries { 0 };
        u32 first_cluster { 0 };
        u32 size { 0 };
        u8 attributes { 0 };
        i64 created { 0 };
        i64 modified { 0 };
        i64 accessed { 0 };
        bool removed { false };
        bool is_directory { false };
        // Walking a long file takes less time with where it last was.
        u32 last_position { 0 };
        u32 last_cluster { 0 };
    };

    struct Found {
        Node node;
        String name;
        u8 short_name[11] {};
    };

    using EntryVisitor = Function<ErrorOr<Iteration>(Found&)>;

    [[nodiscard]] u64 byte_of_cluster(u32 cluster) const
    {
        return m_data_start + static_cast<u64>(cluster - 2) * m_cluster_size;
    }
    [[nodiscard]] bool is_fixed_root(Node const& node) const { return node.parent == 0 && m_bits != Bits::FAT32; }
    [[nodiscard]] bool is_valid_cluster(u32 cluster) const { return cluster >= 2 && cluster - 2 < m_cluster_count; }

    ErrorOr<u32> get_fat(u32 cluster);
    ErrorOr<void> set_fat(u32 cluster, u32 value);
    ErrorOr<u32> allocate_cluster();
    ErrorOr<void> free_chain(u32 first);
    ErrorOr<u32> count_free_clusters();

    ErrorOr<Node*> node_of(NodeIndex);
    [[nodiscard]] NodeInfo info_of(NodeIndex, Node const&) const;
    ErrorOr<NodeIndex> index_of(NodeIndex parent, Found const&);

    // Which cluster holds this position in a chain: only as far as it goes, or further if `extend`.
    ErrorOr<Optional<u32>> cluster_at(Node&, u32 position, bool extend);
    ErrorOr<size_t> read_stream(Node&, u64 offset, Bytes);
    ErrorOr<void> write_stream(Node&, u64 offset, ReadonlyBytes);
    [[nodiscard]] u64 stream_capacity(Node const&);

    ErrorOr<void> walk_directory(Node&, NodeIndex directory, EntryVisitor);
    ErrorOr<Optional<Found>> find(Node& directory, NodeIndex index, StringView name);
    ErrorOr<void> store_entry(Node&);
    ErrorOr<void> remove_entries(Node& directory, u64 short_offset, u32 long_entries);
    ErrorOr<void> make_short_name(Node& directory, NodeIndex index, Span<u16 const> name, u8* output, bool& needs_long);
    // Finds room and writes the entries: where the short one went.
    ErrorOr<void> add_entries(Node& directory, NodeIndex index, Vector<u16> const& name, DirectoryEntry const& entry, bool needs_long, u64& short_offset, u32& long_count);
    ErrorOr<void> shrink(Node&, u64 size);

    ErrorOr<void> mark_changed();
    ErrorOr<void> set_clean(bool);
    [[nodiscard]] i64 now() const { return m_clock ? m_clock() : 0; }

    SectorCache m_cache;
    Clock m_clock;
    bool m_read_only;
    bool m_changed { false };
    Bits m_bits { Bits::FAT32 };
    String m_label;
    u32 m_sector_size { 512 };
    u32 m_cluster_size { 0 };
    u32 m_cluster_count { 0 };
    u32 m_fat_entries { 0 };
    u8 m_fat_count { 0 };
    u8 m_media { 0xf8 };
    u64 m_fat_start { 0 };
    u64 m_fat_size { 0 };
    u64 m_root_start { 0 };
    u32 m_root_entries { 0 };
    u64 m_data_start { 0 };
    u64 m_info_offset { 0 };
    u32 m_next_free { 2 };
    Optional<u32> m_free_clusters;
    // By pointer: a node stays where it is while others come and go.
    HashMap<NodeIndex, Node*> m_nodes;
    // Where someone is, by where it's kept, for it to be the same one when it's found again.
    HashMap<u64, NodeIndex> m_by_location;
    NodeIndex m_next_index { 2 };
};

ErrorOr<void> FATVolume::open()
{
    u8 first[512];
    TRY(m_cache.read(0, { first, sizeof(first) }));
    if (first[510] != 0x55 || first[511] != 0xaa)
        return Error::from_errno(EINVAL);
    BootSector boot;
    __builtin_memcpy(&boot, first, sizeof(boot));
    if (boot.bytes_per_sector < 512 || (boot.bytes_per_sector & (boot.bytes_per_sector - 1)) || boot.bytes_per_sector > 4096)
        return Error::from_errno(EINVAL);
    if (boot.sectors_per_cluster == 0 || (boot.sectors_per_cluster & (boot.sectors_per_cluster - 1)) || boot.fat_count == 0 || boot.reserved_sectors == 0)
        return Error::from_errno(EINVAL);
    m_sector_size = boot.bytes_per_sector;
    m_cluster_size = m_sector_size * boot.sectors_per_cluster;
    m_fat_count = boot.fat_count;
    m_media = boot.media;
    Extended32 extended;
    __builtin_memcpy(&extended, first + sizeof(boot), sizeof(extended));
    u32 const fat_sectors = boot.fat_size_16 ? boot.fat_size_16 : extended.fat_size_32;
    u32 const total_sectors = boot.total_sectors_16 ? boot.total_sectors_16 : boot.total_sectors_32;
    u32 const root_sectors = (boot.root_entry_count * entry_size + m_sector_size - 1) / m_sector_size;
    if (fat_sectors == 0 || total_sectors == 0)
        return Error::from_errno(EINVAL);
    m_fat_start = static_cast<u64>(boot.reserved_sectors) * m_sector_size;
    m_fat_size = static_cast<u64>(fat_sectors) * m_sector_size;
    m_root_start = m_fat_start + m_fat_size * boot.fat_count;
    m_root_entries = boot.root_entry_count;
    m_data_start = m_root_start + static_cast<u64>(root_sectors) * m_sector_size;
    u64 const data_sectors = total_sectors - (m_data_start / m_sector_size);
    m_cluster_count = static_cast<u32>(data_sectors / boot.sectors_per_cluster);
    if (m_cluster_count < 4085)
        m_bits = Bits::FAT12;
    else if (m_cluster_count < 65525)
        m_bits = Bits::FAT16;
    else
        m_bits = Bits::FAT32;
    m_fat_entries = m_cluster_count + 2;
    if (m_data_start + static_cast<u64>(m_cluster_count) * m_cluster_size > m_cache.size())
        return Error::from_errno(EINVAL);

    Node root;
    root.is_directory = true;
    root.attributes = attribute_directory;
    if (m_bits == Bits::FAT32) {
        root.first_cluster = extended.root_cluster;
        if (!is_valid_cluster(root.first_cluster))
            return Error::from_errno(EINVAL);
        m_info_offset = static_cast<u64>(extended.info_sector) * m_sector_size;
    }
    auto* root_copy = new (Base::nothrow) Node(root);
    if (!root_copy)
        return Error::from_errno(ENOMEM);
    TRY(m_nodes.try_set(root_index, root_copy));

    // A label is an entry in the root; there's another copy in the boot sector, but that one goes stale.
    auto const& boot_label = m_bits == Bits::FAT32 ? extended.rest : *reinterpret_cast<Extended16 const*>(&extended);
    char label_text[12] {};
    if (boot_label.signature == 0x29)
        __builtin_memcpy(label_text, boot_label.label, 11);
    auto* root_node = TRY(node_of(root_index));
    size_t root_size = m_bits == Bits::FAT32 ? stream_capacity(*root_node) : m_root_entries * entry_size;
    u8 entry_bytes[entry_size];
    for (u64 offset = 0; offset < root_size; offset += entry_size) {
        if (TRY(read_stream(*root_node, offset, { entry_bytes, entry_size })) != entry_size || entry_bytes[0] == 0)
            break;
        DirectoryEntry entry;
        __builtin_memcpy(&entry, entry_bytes, sizeof(entry));
        if (entry.name[0] != entry_free && entry.attributes != attribute_long && (entry.attributes & attribute_volume)) {
            __builtin_memcpy(label_text, entry.name, 11);
            break;
        }
    }
    size_t label_length = 11;
    while (label_length > 0 && (label_text[label_length - 1] == ' ' || label_text[label_length - 1] == 0))
        --label_length;
    if (StringView(label_text, label_length) == "NO NAME"_sv)
        label_length = 0;
    m_label = TRY(String::from_view({ label_text, label_length }));

    if (m_bits == Bits::FAT32 && m_info_offset != 0 && !m_read_only) {
        u8 info[512];
        TRY(m_cache.read(m_info_offset, { info, sizeof(info) }));
        u32 lead, free_count, next;
        __builtin_memcpy(&lead, info, 4);
        __builtin_memcpy(&free_count, info + 488, 4);
        __builtin_memcpy(&next, info + 492, 4);
        if (lead == fat32_info_lead && next != 0xffffffff && is_valid_cluster(next))
            m_next_free = next;
    }
    return {};
}

ErrorOr<Statistics> FATVolume::statistics()
{
    return Statistics { m_cluster_size, m_cluster_count, TRY(count_free_clusters()) };
}

ErrorOr<u32> FATVolume::get_fat(u32 cluster)
{
    if (cluster >= m_fat_entries)
        return Error::from_errno(EIO);
    switch (m_bits) {
    case Bits::FAT12: {
        u8 bytes[2];
        TRY(m_cache.read(m_fat_start + cluster + cluster / 2, { bytes, 2 }));
        u16 const value = static_cast<u16>(bytes[0] | (bytes[1] << 8));
        return (cluster & 1) ? value >> 4 : value & 0xfff;
    }
    case Bits::FAT16: {
        u8 bytes[2];
        TRY(m_cache.read(m_fat_start + cluster * 2ull, { bytes, 2 }));
        return static_cast<u32>(bytes[0] | (bytes[1] << 8));
    }
    case Bits::FAT32: {
        u32 value;
        TRY(m_cache.read(m_fat_start + cluster * 4ull, { reinterpret_cast<u8*>(&value), 4 }));
        return value & 0x0fffffff;
    }
    }
    return Error::from_errno(EIO);
}

ErrorOr<void> FATVolume::set_fat(u32 cluster, u32 value)
{
    if (cluster >= m_fat_entries)
        return Error::from_errno(EIO);
    // Every copy of it.
    for (u8 copy = 0; copy < m_fat_count; ++copy) {
        u64 const start = m_fat_start + copy * m_fat_size;
        switch (m_bits) {
        case Bits::FAT12: {
            u8 bytes[2];
            u64 const at = start + cluster + cluster / 2;
            TRY(m_cache.read(at, { bytes, 2 }));
            u16 entry = static_cast<u16>(bytes[0] | (bytes[1] << 8));
            if (cluster & 1)
                entry = static_cast<u16>((entry & 0x000f) | (value << 4));
            else
                entry = static_cast<u16>((entry & 0xf000) | (value & 0xfff));
            bytes[0] = static_cast<u8>(entry);
            bytes[1] = static_cast<u8>(entry >> 8);
            TRY(m_cache.write(at, { bytes, 2 }));
            break;
        }
        case Bits::FAT16: {
            u8 const bytes[2] = { static_cast<u8>(value), static_cast<u8>(value >> 8) };
            TRY(m_cache.write(start + cluster * 2ull, { bytes, 2 }));
            break;
        }
        case Bits::FAT32: {
            u32 old;
            TRY(m_cache.read(start + cluster * 4ull, { reinterpret_cast<u8*>(&old), 4 }));
            u32 const entry = (old & 0xf0000000) | (value & 0x0fffffff);
            TRY(m_cache.write(start + cluster * 4ull, { reinterpret_cast<u8 const*>(&entry), 4 }));
            break;
        }
        }
    }
    return {};
}

ErrorOr<u32> FATVolume::count_free_clusters()
{
    if (m_free_clusters.has_value())
        return m_free_clusters.value();
    u32 count = 0;
    for (u32 cluster = 2; cluster < m_fat_entries; ++cluster) {
        if (TRY(get_fat(cluster)) == 0)
            ++count;
    }
    m_free_clusters = count;
    return count;
}

ErrorOr<u32> FATVolume::allocate_cluster()
{
    u32 const free_count = TRY(count_free_clusters());
    if (free_count == 0)
        return Error::from_errno(ENOSPC);
    u32 cluster = is_valid_cluster(m_next_free) ? m_next_free : 2;
    for (u32 tried = 0; tried < m_cluster_count; ++tried) {
        if (TRY(get_fat(cluster)) == 0) {
            TRY(set_fat(cluster, end_of_chain(m_bits)));
            m_free_clusters = free_count - 1;
            m_next_free = cluster + 1 < m_fat_entries ? cluster + 1 : 2;
            return cluster;
        }
        ++cluster;
        if (cluster >= m_fat_entries)
            cluster = 2;
    }
    return Error::from_errno(ENOSPC);
}

ErrorOr<void> FATVolume::free_chain(u32 first)
{
    u32 cluster = first;
    u32 guard = 0;
    u32 free_count = TRY(count_free_clusters());
    while (is_valid_cluster(cluster) && guard++ <= m_cluster_count) {
        u32 const next = TRY(get_fat(cluster));
        TRY(set_fat(cluster, 0));
        ++free_count;
        if (is_end_of_chain(m_bits, next) || next == 0)
            break;
        cluster = next;
    }
    m_free_clusters = free_count;
    return {};
}

ErrorOr<FATVolume::Node*> FATVolume::node_of(NodeIndex index)
{
    auto* node = m_nodes.get(index);
    if (!node)
        return Error::from_errno(ENOENT);
    return *node;
}

NodeInfo FATVolume::info_of(NodeIndex index, Node const& node) const
{
    NodeInfo info;
    info.index = index;
    info.size = node.is_directory ? m_cluster_size : node.size;
    info.allocated = (static_cast<u64>(node.size) + m_cluster_size - 1) / m_cluster_size * m_cluster_size;
    info.is_directory = node.is_directory;
    info.read_only = node.attributes & attribute_read_only;
    info.hidden = node.attributes & attribute_hidden;
    info.created = node.created;
    info.modified = node.modified;
    info.accessed = node.accessed;
    return info;
}

ErrorOr<NodeIndex> FATVolume::index_of(NodeIndex parent, Found const& found)
{
    u64 const location = (parent << 32) ^ found.node.entry_offset;
    if (auto const* known = m_by_location.get(location))
        return *known;
    auto const index = m_next_index++;
    auto* node = new (Base::nothrow) Node(found.node);
    if (!node)
        return Error::from_errno(ENOMEM);
    node->parent = parent;
    TRY(m_nodes.try_set(index, node));
    TRY(m_by_location.try_set(location, index));
    return index;
}

ErrorOr<NodeInfo> FATVolume::info(NodeIndex index)
{
    return info_of(index, *TRY(node_of(index)));
}

ErrorOr<Optional<u32>> FATVolume::cluster_at(Node& node, u32 position, bool extend)
{
    u32 const wanted = position / m_cluster_size;
    u32 cluster = node.first_cluster;
    u32 here = 0;
    if (cluster != 0 && node.last_cluster != 0 && node.last_position <= wanted) {
        cluster = node.last_cluster;
        here = node.last_position;
    }
    if (cluster == 0) {
        if (!extend)
            return Optional<u32> {};
        cluster = TRY(allocate_cluster());
        node.first_cluster = cluster;
        if (node.is_directory)
            TRY(m_cache.zero(byte_of_cluster(cluster), m_cluster_size));
    }
    while (here < wanted) {
        auto next = TRY(get_fat(cluster));
        if (is_end_of_chain(m_bits, next) || next == 0) {
            if (!extend)
                return Optional<u32> {};
            next = TRY(allocate_cluster());
            TRY(set_fat(cluster, next));
            if (node.is_directory)
                TRY(m_cache.zero(byte_of_cluster(next), m_cluster_size));
        } else if (!is_valid_cluster(next) || is_bad_cluster(m_bits, next)) {
            return Error::from_errno(EIO);
        }
        cluster = next;
        ++here;
    }
    node.last_position = here;
    node.last_cluster = cluster;
    return Optional<u32>(cluster);
}

u64 FATVolume::stream_capacity(Node const& node)
{
    if (is_fixed_root(node))
        return static_cast<u64>(m_root_entries) * entry_size;
    if (!node.is_directory)
        return node.size;
    u64 capacity = 0;
    u32 cluster = node.first_cluster;
    u32 guard = 0;
    while (is_valid_cluster(cluster) && guard++ <= m_cluster_count) {
        capacity += m_cluster_size;
        auto next = get_fat(cluster);
        if (next.is_error() || next.value() == 0 || is_end_of_chain(m_bits, next.value()))
            break;
        cluster = next.value();
    }
    return capacity;
}

ErrorOr<size_t> FATVolume::read_stream(Node& node, u64 offset, Bytes buffer)
{
    if (is_fixed_root(node)) {
        u64 const capacity = static_cast<u64>(m_root_entries) * entry_size;
        if (offset >= capacity)
            return 0;
        size_t const part = min<u64>(buffer.size(), capacity - offset);
        TRY(m_cache.read(m_root_start + offset, buffer.trim(part)));
        return part;
    }
    size_t done = 0;
    while (done < buffer.size()) {
        u64 const position = offset + done;
        if (position >= 0x100000000ull)
            break;
        auto const cluster = TRY(cluster_at(node, static_cast<u32>(position), false));
        if (!cluster.has_value())
            break;
        size_t const inside = position % m_cluster_size;
        size_t const part = min<size_t>(buffer.size() - done, m_cluster_size - inside);
        TRY(m_cache.read(byte_of_cluster(cluster.value()) + inside, { buffer.data() + done, part }));
        done += part;
    }
    return done;
}

ErrorOr<void> FATVolume::write_stream(Node& node, u64 offset, ReadonlyBytes buffer)
{
    if (is_fixed_root(node)) {
        if (offset + buffer.size() > static_cast<u64>(m_root_entries) * entry_size)
            return Error::from_errno(ENOSPC);
        return m_cache.write(m_root_start + offset, buffer);
    }
    size_t done = 0;
    while (done < buffer.size()) {
        u64 const position = offset + done;
        auto const cluster = TRY(cluster_at(node, static_cast<u32>(position), true));
        size_t const inside = position % m_cluster_size;
        size_t const part = min<size_t>(buffer.size() - done, m_cluster_size - inside);
        TRY(m_cache.write(byte_of_cluster(cluster.value()) + inside, { buffer.data() + done, part }));
        done += part;
    }
    return {};
}

ErrorOr<void> FATVolume::walk_directory(Node& directory, NodeIndex, EntryVisitor visitor)
{
    u16 long_name[256];
    u32 long_count = 0;
    u8 long_sum = 0;
    size_t long_length = 0;
    u8 next_order = 0;
    u64 const capacity = stream_capacity(directory);
    for (u64 offset = 0; offset < capacity; offset += entry_size) {
        u8 bytes[entry_size];
        if (TRY(read_stream(directory, offset, { bytes, entry_size })) != entry_size)
            break;
        if (bytes[0] == 0)
            break;
        if (bytes[0] == entry_free) {
            long_count = 0;
            continue;
        }
        if (bytes[11] == attribute_long) {
            LongEntry entry;
            __builtin_memcpy(&entry, bytes, sizeof(entry));
            if (entry.order & long_last) {
                long_count = 0;
                long_sum = entry.checksum;
                next_order = entry.order & 0x3f;
                long_length = 0;
            }
            if ((entry.order & 0x3f) != next_order || next_order == 0 || entry.checksum != long_sum) {
                long_count = 0;
                next_order = 0;
                continue;
            }
            size_t const start = static_cast<size_t>(next_order - 1) * 13;
            u16 units[13];
            __builtin_memcpy(units, entry.name1, sizeof(entry.name1));
            __builtin_memcpy(units + 5, entry.name2, sizeof(entry.name2));
            __builtin_memcpy(units + 11, entry.name3, sizeof(entry.name3));
            for (size_t i = 0; i < 13 && start + i < 256; ++i) {
                if (units[i] == 0 || units[i] == 0xffff)
                    break;
                long_name[start + i] = units[i];
                long_length = max(long_length, start + i + 1);
            }
            ++long_count;
            --next_order;
            continue;
        }
        DirectoryEntry entry;
        __builtin_memcpy(&entry, bytes, sizeof(entry));
        if (entry.attributes & attribute_volume) {
            long_count = 0;
            continue;
        }
        Found found;
        __builtin_memcpy(found.short_name, entry.name, 11);
        if (found.short_name[0] == 0x05)
            found.short_name[0] = 0xe5;
        bool const use_long = long_count > 0 && next_order == 0 && long_checksum(entry.name) == long_sum;
        if (use_long) {
            found.name = TRY(utf8_of_utf16({ long_name, long_length }));
            found.node.long_entries = long_count;
        } else {
            char name[13];
            size_t length = 0;
            auto const add_part = [&](size_t from, size_t count, bool lower) {
                size_t last = count;
                while (last > 0 && found.short_name[from + last - 1] == ' ')
                    --last;
                for (size_t i = 0; i < last; ++i) {
                    char c = static_cast<char>(found.short_name[from + i]);
                    if (lower && c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c + 32);
                    name[length++] = c;
                }
            };
            add_part(0, 8, entry.case_flags & case_lower_base);
            if (found.short_name[8] != ' ') {
                name[length++] = '.';
                add_part(8, 3, entry.case_flags & case_lower_extension);
            }
            // (Names outside ASCII, in some old code page, are shown as what they can be.)
            found.name = TRY(utf8_of_utf16(TRY([&]() -> ErrorOr<Vector<u16>> {
                Vector<u16> units;
                for (size_t i = 0; i < length; ++i)
                    TRY(units.try_append(static_cast<u8>(name[i]) < 0x80 ? static_cast<u16>(name[i]) : 0xfffd));
                return units;
            }()).span()));
            found.node.long_entries = 0;
        }
        long_count = 0;
        found.node.entry_offset = offset;
        found.node.first_cluster = (static_cast<u32>(entry.cluster_high) << 16 & (m_bits == Bits::FAT32 ? 0xffffffffu : 0u)) | entry.cluster_low;
        found.node.size = entry.size;
        found.node.attributes = entry.attributes;
        found.node.is_directory = entry.attributes & attribute_directory;
        found.node.created = seconds_of(entry.creation_date, entry.creation_time);
        found.node.modified = seconds_of(entry.modification_date, entry.modification_time);
        found.node.accessed = seconds_of(entry.access_date, 0);
        if (TRY(visitor(found)) == Iteration::Stop)
            break;
    }
    return {};
}

ErrorOr<Optional<FATVolume::Found>> FATVolume::find(Node& directory, NodeIndex index, StringView name)
{
    auto const wanted = TRY(comparable_name(name));
    Optional<Found> result;
    TRY(walk_directory(directory, index, [&](Found& found) -> ErrorOr<Iteration> {
        auto const here = TRY(comparable_name(found.name.view()));
        if (here.size() == wanted.size() && __builtin_memcmp(here.begin(), wanted.begin(), wanted.size() * sizeof(u16)) == 0) {
            result = move(found);
            return Iteration::Stop;
        }
        return Iteration::Continue;
    }));
    return result;
}

ErrorOr<NodeInfo> FATVolume::lookup(NodeIndex directory_index, StringView name)
{
    auto& directory = *TRY(node_of(directory_index));
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    if (name == "."_sv)
        return info_of(directory_index, directory);
    if (name == ".."_sv) {
        NodeIndex const parent = directory.parent ? directory.parent : directory_index;
        return info_of(parent, *TRY(node_of(parent)));
    }
    auto found = TRY(find(directory, directory_index, name));
    if (!found.has_value())
        return Error::from_errno(ENOENT);
    auto const index = TRY(index_of(directory_index, found.value()));
    return info_of(index, *TRY(node_of(index)));
}

ErrorOr<void> FATVolume::for_each_entry(NodeIndex directory_index, Function<Iteration(StringView, NodeInfo const&)> callback)
{
    auto& directory = *TRY(node_of(directory_index));
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    return walk_directory(directory, directory_index, [&](Found& found) -> ErrorOr<Iteration> {
        if (found.name == "."_sv || found.name == ".."_sv)
            return Iteration::Continue;
        auto const index = TRY(index_of(directory_index, found));
        return callback(found.name.view(), info_of(index, *TRY(node_of(index))));
    });
}

ErrorOr<size_t> FATVolume::read(NodeIndex index, u64 offset, Bytes buffer)
{
    auto& node = *TRY(node_of(index));
    if (node.is_directory)
        return Error::from_errno(EISDIR);
    if (offset >= node.size)
        return 0;
    size_t const count = min<u64>(buffer.size(), node.size - offset);
    return read_stream(node, offset, buffer.trim(count));
}

ErrorOr<void> FATVolume::mark_changed()
{
    if (m_read_only)
        return Error::from_errno(EROFS);
    if (!m_changed) {
        m_changed = true;
        TRY(set_clean(false));
    }
    return {};
}

// The second entry of the FAT says if it was left in good order: FAT12 hasn't got this.
ErrorOr<void> FATVolume::set_clean(bool clean)
{
    if (m_bits == Bits::FAT12)
        return {};
    u32 const bit = m_bits == Bits::FAT16 ? 0x8000 : 0x08000000;
    u32 value = TRY(get_fat(1));
    value = clean ? value | bit : value & ~bit;
    return set_fat(1, value);
}

ErrorOr<size_t> FATVolume::write(NodeIndex index, u64 offset, ReadonlyBytes buffer)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    if (node.is_directory)
        return Error::from_errno(EISDIR);
    if (node.attributes & attribute_read_only)
        return Error::from_errno(EACCES);
    if (buffer.is_empty())
        return 0;
    if (offset + buffer.size() > most_file_size)
        return Error::from_errno(EFBIG);
    if (offset > node.size)
        TRY(truncate(index, offset));
    TRY(write_stream(node, offset, buffer));
    if (offset + buffer.size() > node.size)
        node.size = static_cast<u32>(offset + buffer.size());
    node.modified = now();
    node.accessed = node.modified;
    node.attributes |= attribute_archive;
    TRY(store_entry(node));
    return buffer.size();
}

ErrorOr<void> FATVolume::shrink(Node& node, u64 size)
{
    if (size == 0) {
        if (node.first_cluster)
            TRY(free_chain(node.first_cluster));
        node.first_cluster = 0;
    } else {
        auto const last = TRY(cluster_at(node, static_cast<u32>(size - 1), false));
        if (!last.has_value())
            return Error::from_errno(EIO);
        auto const next = TRY(get_fat(last.value()));
        if (!is_end_of_chain(m_bits, next) && next != 0)
            TRY(free_chain(next));
        TRY(set_fat(last.value(), end_of_chain(m_bits)));
    }
    node.last_cluster = 0;
    node.last_position = 0;
    return {};
}

ErrorOr<void> FATVolume::truncate(NodeIndex index, u64 size)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    if (node.is_directory)
        return Error::from_errno(EISDIR);
    if (size > most_file_size)
        return Error::from_errno(EFBIG);
    if (size == node.size)
        return {};
    if (size < node.size) {
        TRY(shrink(node, size));
    } else {
        // What's new reads as zeros.
        u64 const from = node.size;
        if (from % m_cluster_size != 0) {
            auto const cluster = TRY(cluster_at(node, static_cast<u32>(from - 1), false));
            if (cluster.has_value()) {
                u64 const inside = from % m_cluster_size;
                TRY(m_cache.zero(byte_of_cluster(cluster.value()) + inside, m_cluster_size - inside));
            }
        }
        u64 position = (from + m_cluster_size - 1) / m_cluster_size * m_cluster_size;
        while (position < size) {
            auto const cluster = TRY(cluster_at(node, static_cast<u32>(position), true));
            TRY(m_cache.zero(byte_of_cluster(cluster.value()), m_cluster_size));
            position += m_cluster_size;
        }
    }
    node.size = static_cast<u32>(size);
    node.modified = now();
    node.attributes |= attribute_archive;
    return store_entry(node);
}

ErrorOr<void> FATVolume::store_entry(Node& node)
{
    if (node.parent == 0 || node.removed)
        return {};
    auto& parent = *TRY(node_of(node.parent));
    u8 bytes[entry_size];
    if (TRY(read_stream(parent, node.entry_offset, { bytes, entry_size })) != entry_size)
        return Error::from_errno(EIO);
    DirectoryEntry entry;
    __builtin_memcpy(&entry, bytes, sizeof(entry));
    entry.attributes = node.attributes;
    entry.size = node.is_directory ? 0 : node.size;
    entry.cluster_low = static_cast<u16>(node.first_cluster);
    entry.cluster_high = m_bits == Bits::FAT32 ? static_cast<u16>(node.first_cluster >> 16) : 0;
    entry.modification_date = dos_date_of(node.modified);
    entry.modification_time = dos_time_of(node.modified);
    entry.access_date = dos_date_of(node.accessed);
    __builtin_memcpy(bytes, &entry, sizeof(entry));
    return write_stream(parent, node.entry_offset, { bytes, entry_size });
}

ErrorOr<void> FATVolume::make_short_name(Node& directory, NodeIndex index, Span<u16 const> name, u8* output, bool& needs_long)
{
    __builtin_memset(output, ' ', 11);
    // Where the extension starts: after the last dot, if it isn't first.
    size_t dot = name.size();
    for (size_t i = name.size(); i > 0; --i) {
        if (name[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    if (dot == 0)
        dot = name.size();
    bool lossy = false;
    bool any_lower = false;
    size_t base_length = 0;
    auto const clean = [&](u16 unit) -> u8 {
        if (unit >= 'a' && unit <= 'z') {
            any_lower = true;
            return static_cast<u8>(unit - 32);
        }
        bool const allowed = (unit >= 'A' && unit <= 'Z') || (unit >= '0' && unit <= '9') || is_one_of("!#$%&'()-@^_`{}~"_sv, unit);
        if (allowed)
            return static_cast<u8>(unit);
        lossy = true;
        return '_';
    };
    for (size_t i = 0; i < dot; ++i) {
        if (name[i] == ' ' || name[i] == '.') {
            lossy = true;
            continue;
        }
        if (base_length == 8) {
            lossy = true;
            continue;
        }
        output[base_length++] = clean(name[i]);
    }
    size_t extension_length = 0;
    for (size_t i = dot + 1; i < name.size(); ++i) {
        if (name[i] == ' ' || name[i] == '.') {
            lossy = true;
            continue;
        }
        if (extension_length == 3) {
            lossy = true;
            continue;
        }
        output[8 + extension_length++] = clean(name[i]);
    }
    if (dot < name.size() && dot > 8)
        lossy = true;
    needs_long = lossy || any_lower;
    if (output[0] == 0xe5)
        output[0] = 0x05;
    // Is that one taken? If it can't be exactly that, it's a ~N.
    Vector<Array<u8, 11>> taken;
    TRY(walk_directory(directory, index, [&](Found& found) -> ErrorOr<Iteration> {
        Array<u8, 11> copy;
        __builtin_memcpy(copy.data(), found.short_name, 11);
        TRY(taken.try_append(copy));
        return Iteration::Continue;
    }));
    auto const is_taken = [&](u8 const* candidate) {
        for (auto const& other : taken) {
            if (__builtin_memcmp(other.data(), candidate, 11) == 0)
                return true;
        }
        return false;
    };
    if (!lossy && !is_taken(output))
        return {};
    needs_long = true;
    u8 candidate[11];
    for (u32 number = 1; number < 1000000; ++number) {
        char digits[8];
        size_t digit_count = 0;
        for (u32 rest = number; rest; rest /= 10)
            digits[digit_count++] = static_cast<char>('0' + rest % 10);
        __builtin_memcpy(candidate, output, 11);
        size_t const keep = min<size_t>(max<size_t>(base_length, 1), 8 - digit_count - 1);
        candidate[keep] = '~';
        for (size_t i = 0; i < digit_count; ++i)
            candidate[keep + 1 + i] = static_cast<u8>(digits[digit_count - 1 - i]);
        for (size_t i = keep + 1 + digit_count; i < 8; ++i)
            candidate[i] = ' ';
        if (!is_taken(candidate)) {
            __builtin_memcpy(output, candidate, 11);
            return {};
        }
    }
    return Error::from_errno(EEXIST);
}

ErrorOr<void> FATVolume::add_entries(Node& directory, NodeIndex index, Vector<u16> const& name, DirectoryEntry const& entry, bool needs_long, u64& short_offset, u32& long_count)
{
    long_count = needs_long ? static_cast<u32>((name.size() + 12) / 13) : 0;
    u32 const wanted = long_count + 1;
    // A run of free entries; or the end, past which everything's free.
    u64 run_start = 0;
    u32 run = 0;
    u64 const capacity = stream_capacity(directory);
    u64 offset = 0;
    bool at_end = false;
    for (; offset < capacity; offset += entry_size) {
        u8 first;
        TRY(read_stream(directory, offset, { &first, 1 }));
        if (first == 0) {
            at_end = true;
            break;
        }
        if (first == entry_free) {
            if (run == 0)
                run_start = offset;
            if (++run == wanted)
                break;
        } else {
            run = 0;
        }
    }
    if (at_end) {
        if (run == 0)
            run_start = offset;
        run = wanted;
    } else if (run < wanted) {
        if (is_fixed_root(directory))
            return Error::from_errno(ENOSPC);
        // Starts a new cluster's worth, or carries on from the free ones at the end.
        if (run == 0)
            run_start = offset;
        run = wanted;
    }
    if (is_fixed_root(directory) && run_start + wanted * entry_size > static_cast<u64>(m_root_entries) * entry_size)
        return Error::from_errno(ENOSPC);
    // (FAT32's limit for a directory: 65536 entries.)
    if (run_start + wanted * entry_size > 65536 * entry_size)
        return Error::from_errno(ENOSPC);

    u8 const sum = long_checksum(entry.name);
    for (u32 i = 0; i < long_count; ++i) {
        u32 const order = long_count - i;
        size_t const start = (order - 1) * 13;
        u16 units[13];
        for (size_t j = 0; j < 13; ++j) {
            size_t const at = start + j;
            if (at < name.size())
                units[j] = name[at];
            else if (at == name.size())
                units[j] = 0;
            else
                units[j] = 0xffff;
        }
        LongEntry long_entry {};
        long_entry.order = static_cast<u8>(order | (i == 0 ? long_last : 0));
        __builtin_memcpy(long_entry.name1, units, sizeof(long_entry.name1));
        long_entry.attributes = attribute_long;
        long_entry.checksum = sum;
        __builtin_memcpy(long_entry.name2, units + 5, sizeof(long_entry.name2));
        __builtin_memcpy(long_entry.name3, units + 11, sizeof(long_entry.name3));
        u8 bytes[entry_size];
        __builtin_memcpy(bytes, &long_entry, sizeof(bytes));
        TRY(write_stream(directory, run_start + i * entry_size, { bytes, entry_size }));
    }
    short_offset = run_start + long_count * entry_size;
    u8 bytes[entry_size];
    __builtin_memcpy(bytes, &entry, sizeof(bytes));
    TRY(write_stream(directory, short_offset, { bytes, entry_size }));
    (void)index;
    return {};
}

ErrorOr<NodeInfo> FATVolume::create(NodeIndex directory_index, StringView name, bool is_directory)
{
    TRY(mark_changed());
    auto& directory = *TRY(node_of(directory_index));
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    if (TRY(find(directory, directory_index, name)).has_value())
        return Error::from_errno(EEXIST);
    if (name.is_empty() || name == "."_sv || name == ".."_sv)
        return Error::from_errno(EINVAL);
    auto const normalized = TRY(Unicode::normalize(name, Unicode::NormalizationForm::NFC));
    auto const units = TRY(utf16_of(normalized.view()));
    if (units.size() > 255)
        return Error::from_errno(ENAMETOOLONG);
    for (auto unit : units) {
        if (unit < 0x20 || is_one_of("\"*/:<>?\\|"_sv, unit))
            return Error::from_errno(EINVAL);
    }
    // A name can't end in a space or a dot: it wouldn't come back the same anywhere else.
    if (units.last() == ' ' || units.last() == '.')
        return Error::from_errno(EINVAL);

    i64 const time = now();
    DirectoryEntry entry {};
    bool needs_long = false;
    TRY(make_short_name(directory, directory_index, { units.begin(), units.size() }, entry.name, needs_long));
    entry.attributes = is_directory ? attribute_directory : attribute_archive;
    entry.creation_date = dos_date_of(time);
    entry.creation_time = dos_time_of(time);
    entry.modification_date = entry.creation_date;
    entry.modification_time = entry.creation_time;
    entry.access_date = entry.creation_date;

    u32 first_cluster = 0;
    if (is_directory) {
        first_cluster = TRY(allocate_cluster());
        TRY(m_cache.zero(byte_of_cluster(first_cluster), m_cluster_size));
        DirectoryEntry dot = entry;
        __builtin_memset(dot.name, ' ', 11);
        dot.name[0] = '.';
        dot.cluster_low = static_cast<u16>(first_cluster);
        dot.cluster_high = m_bits == Bits::FAT32 ? static_cast<u16>(first_cluster >> 16) : 0;
        DirectoryEntry dot_dot = dot;
        dot_dot.name[1] = '.';
        // The parent, or none if that's the root, whichever kind it is.
        u32 const parent_cluster = directory.parent == 0 ? 0 : directory.first_cluster;
        dot_dot.cluster_low = static_cast<u16>(parent_cluster);
        dot_dot.cluster_high = m_bits == Bits::FAT32 ? static_cast<u16>(parent_cluster >> 16) : 0;
        TRY(m_cache.write(byte_of_cluster(first_cluster), { reinterpret_cast<u8 const*>(&dot), entry_size }));
        TRY(m_cache.write(byte_of_cluster(first_cluster) + entry_size, { reinterpret_cast<u8 const*>(&dot_dot), entry_size }));
        entry.cluster_low = dot.cluster_low;
        entry.cluster_high = dot.cluster_high;
    }

    u64 short_offset = 0;
    u32 long_count = 0;
    TRY(add_entries(directory, directory_index, units, entry, needs_long, short_offset, long_count));
    directory.modified = time;
    TRY(store_entry(directory));

    Found found;
    found.node.entry_offset = short_offset;
    found.node.long_entries = long_count;
    found.node.first_cluster = first_cluster;
    found.node.attributes = entry.attributes;
    found.node.is_directory = is_directory;
    found.node.created = seconds_of(entry.creation_date, entry.creation_time);
    found.node.modified = found.node.created;
    found.node.accessed = found.node.created;
    found.name = TRY(String::from_view(name));
    // (Something earlier in that place, removed, may still be known by it.)
    m_by_location.remove((directory_index << 32) ^ short_offset);
    auto const index = TRY(index_of(directory_index, found));
    return info_of(index, *TRY(node_of(index)));
}

ErrorOr<void> FATVolume::remove_entries(Node& directory, u64 short_offset, u32 long_entries)
{
    u8 const free_mark = entry_free;
    for (u32 i = 0; i <= long_entries; ++i)
        TRY(write_stream(directory, short_offset - i * entry_size, { &free_mark, 1 }));
    return {};
}

ErrorOr<void> FATVolume::remove(NodeIndex directory_index, StringView name)
{
    TRY(mark_changed());
    auto& directory = *TRY(node_of(directory_index));
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    auto found = TRY(find(directory, directory_index, name));
    if (!found.has_value())
        return Error::from_errno(ENOENT);
    auto const index = TRY(index_of(directory_index, found.value()));
    auto& node = *TRY(node_of(index));
    if (node.is_directory) {
        bool empty = true;
        TRY(walk_directory(node, index, [&](Found& inner) -> ErrorOr<Iteration> {
            if (inner.name == "."_sv || inner.name == ".."_sv)
                return Iteration::Continue;
            empty = false;
            return Iteration::Stop;
        }));
        if (!empty)
            return Error::from_errno(ENOTEMPTY);
    }
    if (node.attributes & attribute_read_only)
        return Error::from_errno(EACCES);
    TRY(remove_entries(directory, node.entry_offset, node.long_entries));
    m_by_location.remove((directory_index << 32) ^ node.entry_offset);
    node.removed = true;
    directory.modified = now();
    return store_entry(directory);
}

ErrorOr<void> FATVolume::rename(NodeIndex from_directory_index, StringView from_name, NodeIndex to_directory_index, StringView to_name)
{
    TRY(mark_changed());
    auto& from_directory = *TRY(node_of(from_directory_index));
    auto& to_directory = *TRY(node_of(to_directory_index));
    if (!from_directory.is_directory || !to_directory.is_directory)
        return Error::from_errno(ENOTDIR);
    auto found = TRY(find(from_directory, from_directory_index, from_name));
    if (!found.has_value())
        return Error::from_errno(ENOENT);
    auto const index = TRY(index_of(from_directory_index, found.value()));
    auto& moving = *TRY(node_of(index));
    if (moving.attributes & attribute_read_only)
        return Error::from_errno(EACCES);

    // Into itself? Going up from where it's going shouldn't come to it.
    if (moving.is_directory) {
        for (NodeIndex walk = to_directory_index; walk != 0;) {
            if (walk == index)
                return Error::from_errno(EINVAL);
            walk = TRY(node_of(walk))->parent;
        }
    }

    auto const normalized = TRY(Unicode::normalize(to_name, Unicode::NormalizationForm::NFC));
    auto const units = TRY(utf16_of(normalized.view()));
    if (units.size() > 255)
        return Error::from_errno(ENAMETOOLONG);
    for (auto unit : units) {
        if (unit < 0x20 || is_one_of("\"*/:<>?\\|"_sv, unit))
            return Error::from_errno(EINVAL);
    }

    auto existing = TRY(find(to_directory, to_directory_index, to_name));
    bool const same_thing = existing.has_value() && from_directory_index == to_directory_index && existing.value().node.entry_offset == moving.entry_offset;
    if (existing.has_value() && !same_thing) {
        auto const replaced = TRY(index_of(to_directory_index, existing.value()));
        auto const& replaced_node = *TRY(node_of(replaced));
        if (replaced_node.is_directory && !moving.is_directory)
            return Error::from_errno(EISDIR);
        if (!replaced_node.is_directory && moving.is_directory)
            return Error::from_errno(ENOTDIR);
        // (It goes, as with any other removing, when what's using it has finished.)
        TRY(remove(to_directory_index, to_name));
    }

    u8 bytes[entry_size];
    if (TRY(read_stream(from_directory, moving.entry_offset, { bytes, entry_size })) != entry_size)
        return Error::from_errno(EIO);
    DirectoryEntry entry;
    __builtin_memcpy(&entry, bytes, sizeof(entry));

    // Off first, so it can't get in the way of its own new spelling.
    u64 const old_offset = moving.entry_offset;
    TRY(remove_entries(from_directory, old_offset, moving.long_entries));
    m_by_location.remove((from_directory_index << 32) ^ old_offset);

    bool needs_long = false;
    TRY(make_short_name(to_directory, to_directory_index, { units.begin(), units.size() }, entry.name, needs_long));
    entry.case_flags = 0;
    u64 short_offset = 0;
    u32 long_count = 0;
    TRY(add_entries(to_directory, to_directory_index, units, entry, needs_long, short_offset, long_count));
    moving.parent = to_directory_index;
    moving.entry_offset = short_offset;
    moving.long_entries = long_count;
    m_by_location.remove((to_directory_index << 32) ^ short_offset);
    TRY(m_by_location.try_set((to_directory_index << 32) ^ short_offset, index));

    // A directory's ".." says where it is.
    if (moving.is_directory && from_directory_index != to_directory_index) {
        u32 const parent_cluster = to_directory.parent == 0 ? 0 : to_directory.first_cluster;
        u8 dot_dot_bytes[entry_size];
        TRY(m_cache.read(byte_of_cluster(moving.first_cluster) + entry_size, { dot_dot_bytes, entry_size }));
        DirectoryEntry dot_dot;
        __builtin_memcpy(&dot_dot, dot_dot_bytes, sizeof(dot_dot));
        dot_dot.cluster_low = static_cast<u16>(parent_cluster);
        dot_dot.cluster_high = m_bits == Bits::FAT32 ? static_cast<u16>(parent_cluster >> 16) : 0;
        TRY(m_cache.write(byte_of_cluster(moving.first_cluster) + entry_size, { reinterpret_cast<u8 const*>(&dot_dot), entry_size }));
    }
    i64 const time = now();
    from_directory.modified = time;
    to_directory.modified = time;
    TRY(store_entry(from_directory));
    return store_entry(to_directory);
}

ErrorOr<void> FATVolume::set_times(NodeIndex index, i64 accessed, i64 modified)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    node.accessed = accessed;
    node.modified = modified;
    return store_entry(node);
}

ErrorOr<void> FATVolume::set_read_only(NodeIndex index, bool read_only)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    node.attributes = read_only ? node.attributes | attribute_read_only : node.attributes & ~attribute_read_only;
    return store_entry(node);
}

void FATVolume::forget(NodeIndex index)
{
    if (index == root_index)
        return;
    auto* found = m_nodes.get(index);
    if (!found)
        return;
    auto* node = *found;
    if (node->removed) {
        if (node->first_cluster)
            (void)free_chain(node->first_cluster);
    } else {
        m_by_location.remove((node->parent << 32) ^ node->entry_offset);
    }
    m_nodes.remove(index);
    delete node;
}

ErrorOr<void> FATVolume::flush()
{
    if (!m_changed)
        return {};
    if (m_bits == Bits::FAT32 && m_info_offset != 0) {
        u8 info[512];
        TRY(m_cache.read(m_info_offset, { info, sizeof(info) }));
        u32 const free_count = TRY(count_free_clusters());
        __builtin_memcpy(info + 488, &free_count, 4);
        __builtin_memcpy(info + 492, &m_next_free, 4);
        TRY(m_cache.write(m_info_offset, { info, sizeof(info) }));
    }
    TRY(set_clean(true));
    TRY(m_cache.flush());
    m_changed = false;
    return {};
}

ErrorOr<Extent> FATVolume::extent(NodeIndex index)
{
    auto& node = *TRY(node_of(index));
    if (node.is_directory || node.first_cluster == 0 || node.size == 0)
        return Error::from_errno(EINVAL);
    // Each cluster must be followed by the next one along, as far as the file goes.
    u32 const clusters = (node.size + m_cluster_size - 1) / m_cluster_size;
    u32 cluster = node.first_cluster;
    for (u32 i = 1; i < clusters; ++i) {
        auto const next = TRY(get_fat(cluster));
        if (next != cluster + 1)
            return Error::from_errno(EFBIG);
        cluster = next;
    }
    return Extent { byte_of_cluster(node.first_cluster) / Store::sector_size, node.size };
}

ErrorOr<void> FATVolume::repair(Function<void(StringView)> say)
{
    if (m_read_only)
        return Error::from_errno(EROFS);
    m_changed = true;
    Vector<u8> seen;
    TRY(seen.try_resize(m_fat_entries));
    __builtin_memset(seen.begin(), 0, seen.size());
    auto const tell = [&](StringView what, StringView name) {
        StringBuilder text;
        text.appendff("{}: {}", name, what);
        auto const line = text.to_string();
        if (!line.is_error() && say)
            say(line.value().view());
    };
    // A chain, cut where it stops making sense. How many clusters it came to.
    auto const fix_chain = [&](Node& node, StringView name) -> ErrorOr<u64> {
        u32 cluster = node.first_cluster;
        u32 previous = 0;
        u64 count = 0;
        while (cluster != 0 && !is_end_of_chain(m_bits, cluster)) {
            if (!is_valid_cluster(cluster) || is_bad_cluster(m_bits, cluster) || seen[cluster]) {
                tell("its chain was cut short"_sv, name);
                if (previous)
                    TRY(set_fat(previous, end_of_chain(m_bits)));
                else
                    node.first_cluster = 0;
                break;
            }
            seen[cluster] = 1;
            ++count;
            previous = cluster;
            cluster = TRY(get_fat(cluster));
            if (cluster == 0) {
                tell("its chain was cut short"_sv, name);
                TRY(set_fat(previous, end_of_chain(m_bits)));
                break;
            }
        }
        return count;
    };
    Vector<NodeIndex> pending;
    TRY(pending.try_append(root_index));
    if (m_bits == Bits::FAT32)
        (void)TRY(fix_chain(*TRY(node_of(root_index)), "the root"_sv));
    while (!pending.is_empty()) {
        auto const index = pending.take_last();
        auto& directory = *TRY(node_of(index));
        Vector<NodeIndex> inside;
        TRY(walk_directory(directory, index, [&](Found& found) -> ErrorOr<Iteration> {
            if (found.name == "."_sv || found.name == ".."_sv)
                return Iteration::Continue;
            TRY(inside.try_append(TRY(index_of(index, found))));
            return Iteration::Continue;
        }));
        for (auto inner : inside) {
            auto& node = *TRY(node_of(inner));
            StringView const name = node.is_directory ? "a folder"_sv : "a file"_sv;
            u64 const count = TRY(fix_chain(node, name));
            if (node.is_directory) {
                TRY(pending.try_append(inner));
            } else {
                u64 const needed = (static_cast<u64>(node.size) + m_cluster_size - 1) / m_cluster_size;
                if (count < needed) {
                    tell("it was longer than its clusters, so it's shorter now"_sv, name);
                    node.size = static_cast<u32>(count * m_cluster_size);
                } else if (count > needed) {
                    tell("it had more clusters than it needed"_sv, name);
                    node.size = static_cast<u32>(min<u64>(count * m_cluster_size, 0xffffffffull));
                }
            }
            node.last_cluster = 0;
            node.last_position = 0;
            TRY(store_entry(node));
        }
    }
    u32 lost = 0;
    for (u32 cluster = 2; cluster < m_fat_entries; ++cluster) {
        u32 const value = TRY(get_fat(cluster));
        if (value != 0 && !seen[cluster] && !is_bad_cluster(m_bits, value)) {
            TRY(set_fat(cluster, 0));
            ++lost;
        }
    }
    if (lost) {
        StringBuilder text;
        text.appendff("{} clusters that nothing was using are free again", lost);
        auto const line = TRY(text.to_string());
        if (say)
            say(line.view());
    }
    // All copies of the FAT from the first; and it's counted anew.
    if (m_fat_count > 1) {
        auto* piece = static_cast<u8*>(operator new[](SectorCache::piece_size, Base::nothrow));
        if (!piece)
            return Error::from_errno(ENOMEM);
        for (u64 at = 0; at < m_fat_size; at += SectorCache::piece_size) {
            size_t const part = min<u64>(SectorCache::piece_size, m_fat_size - at);
            auto const read_done = m_cache.read(m_fat_start + at, { piece, part });
            if (!read_done.is_error()) {
                for (u8 copy = 1; copy < m_fat_count; ++copy)
                    (void)m_cache.write(m_fat_start + copy * m_fat_size + at, { piece, part });
            }
        }
        operator delete[](piece, Base::nothrow);
    }
    m_free_clusters = {};
    return flush();
}

ErrorOr<CheckReport> FATVolume::check()
{
    CheckReport report;
    auto const problem = [&](StringView text) -> ErrorOr<void> {
        return report.problems.try_append(TRY(String::from_view(text)));
    };
    // Which cluster has been seen where: walking every directory down from the root, and every chain from each.
    Vector<u8> seen;
    TRY(seen.try_resize(m_fat_entries));
    __builtin_memset(seen.begin(), 0, seen.size());
    auto const walk_chain = [&](u32 first, u64 expected_size, bool directory, StringView name) -> ErrorOr<void> {
        u32 cluster = first;
        u64 count = 0;
        while (cluster != 0 && !is_end_of_chain(m_bits, cluster)) {
            if (!is_valid_cluster(cluster) || is_bad_cluster(m_bits, cluster)) {
                StringBuilder text;
                text.appendff("{}: its chain goes outside the volume", name);
                TRY(problem(text.view()));
                return {};
            }
            if (seen[cluster]) {
                StringBuilder text;
                text.appendff("{}: cluster {} is used twice", name, cluster);
                TRY(problem(text.view()));
                return {};
            }
            seen[cluster] = 1;
            ++count;
            cluster = TRY(get_fat(cluster));
            if (cluster == 0) {
                StringBuilder text;
                text.appendff("{}: its chain has a free cluster in it", name);
                TRY(problem(text.view()));
                return {};
            }
        }
        if (!directory) {
            u64 const needed = (expected_size + m_cluster_size - 1) / m_cluster_size;
            if (needed != count) {
                StringBuilder text;
                text.appendff("{}: has {} clusters, and it should have {}", name, count, needed);
                TRY(problem(text.view()));
            }
        }
        return {};
    };

    Vector<NodeIndex> pending;
    TRY(pending.try_append(root_index));
    if (m_bits == Bits::FAT32)
        TRY(walk_chain(TRY(node_of(root_index))->first_cluster, 0, true, "the root"_sv));
    while (!pending.is_empty()) {
        auto const index = pending.take_last();
        auto& directory = *TRY(node_of(index));
        Vector<NodeIndex> found_directories;
        TRY(walk_directory(directory, index, [&](Found& found) -> ErrorOr<Iteration> {
            if (found.name == "."_sv || found.name == ".."_sv)
                return Iteration::Continue;
            auto const inner = TRY(index_of(index, found));
            if (found.node.is_directory) {
                ++report.directories;
                TRY(walk_chain(found.node.first_cluster, 0, true, found.name.view()));
                TRY(found_directories.try_append(inner));
            } else {
                ++report.files;
                TRY(walk_chain(found.node.first_cluster, found.node.size, false, found.name.view()));
            }
            return Iteration::Continue;
        }));
        for (auto inner : found_directories)
            TRY(pending.try_append(inner));
    }
    u32 lost = 0;
    u32 free_count = 0;
    for (u32 cluster = 2; cluster < m_fat_entries; ++cluster) {
        u32 const value = TRY(get_fat(cluster));
        if (value == 0)
            ++free_count;
        else if (!seen[cluster] && !is_bad_cluster(m_bits, value))
            ++lost;
    }
    if (lost) {
        StringBuilder text;
        text.appendff("{} clusters are in use, but by nothing", lost);
        TRY(problem(text.view()));
    }
    // The copies of the FAT should say the same.
    if (m_fat_count > 1) {
        auto* first = static_cast<u8*>(operator new[](SectorCache::piece_size, Base::nothrow));
        auto* second = static_cast<u8*>(operator new[](SectorCache::piece_size, Base::nothrow));
        if (!first || !second)
            return Error::from_errno(ENOMEM);
        bool differ = false;
        for (u64 at = 0; at < m_fat_size && !differ; at += SectorCache::piece_size) {
            size_t const part = min<u64>(SectorCache::piece_size, m_fat_size - at);
            TRY(m_cache.read(m_fat_start + at, { first, part }));
            TRY(m_cache.read(m_fat_start + m_fat_size + at, { second, part }));
            differ = __builtin_memcmp(first, second, part) != 0;
        }
        operator delete[](first, Base::nothrow);
        operator delete[](second, Base::nothrow);
        if (differ)
            TRY(problem("the copies of the FAT differ"_sv));
    }
    if (m_bits == Bits::FAT32 && m_info_offset != 0) {
        u8 info[512];
        TRY(m_cache.read(m_info_offset, { info, sizeof(info) }));
        u32 said;
        __builtin_memcpy(&said, info + 488, 4);
        if (said != 0xffffffff && said != free_count) {
            StringBuilder text;
            text.appendff("it says {} clusters are free, and there are {}", said, free_count);
            TRY(problem(text.view()));
        }
    }
    if (m_bits != Bits::FAT12) {
        u32 const bit = m_bits == Bits::FAT16 ? 0x8000 : 0x08000000;
        if (!(TRY(get_fat(1)) & bit))
            TRY(problem("it wasn't put away in good order"_sv));
    }
    return report;
}

}

ErrorOr<NonnullOwnPtr<Volume>> open_fat(Store& store, Clock clock, bool read_only)
{
    auto* volume = new (Base::nothrow) FATVolume(store, clock, read_only);
    if (!volume)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*static_cast<Volume*>(volume));
    TRY(volume->open());
    return owned;
}

ErrorOr<CheckReport> check_fat(Store& store, Clock clock)
{
    FATVolume volume(store, clock, true);
    TRY(volume.open());
    return volume.check();
}

ErrorOr<void> format_fat(Store& store, StringView label, Clock clock, unsigned bits)
{
    u64 const sectors = store.sector_count();
    u64 const megabytes = sectors * Store::sector_size / (1024 * 1024);
    if (sectors < 128)
        return Error::from_errno(ENOSPC);
    if (bits == 0)
        bits = megabytes < 16 ? 12 : megabytes <= 2048 ? 16 : 32;
    u32 sectors_per_cluster;
    u32 reserved;
    u32 root_entries;
    if (bits == 12) {
        sectors_per_cluster = 2;
        // As few as still leaves fewer clusters than FAT12 can count.
        while (sectors / sectors_per_cluster >= 4000 && sectors_per_cluster < 128)
            sectors_per_cluster *= 2;
        reserved = 1;
        root_entries = 512;
    } else if (bits == 16) {
        if (megabytes > 2048 || megabytes < 8)
            return Error::from_errno(EINVAL);
        sectors_per_cluster = megabytes <= 16 ? 2 : megabytes <= 128 ? 4 : megabytes <= 256 ? 8 : megabytes <= 512 ? 16 : megabytes <= 1024 ? 32 : 64;
        reserved = 1;
        root_entries = 512;
    } else if (bits == 32) {
        if (megabytes < 33)
            return Error::from_errno(EINVAL);
        sectors_per_cluster = megabytes <= 260 ? 1 : megabytes <= 8192 ? 8 : megabytes <= 16384 ? 16 : megabytes <= 32768 ? 32 : 64;
        reserved = 32;
        root_entries = 0;
    } else {
        return Error::from_errno(EINVAL);
    }
    u64 const total = min<u64>(sectors, 0xffffffffull);
    u64 const root_sectors = (root_entries * entry_size + Store::sector_size - 1) / Store::sector_size;
    // From Microsoft's document.
    u64 const left = total - reserved - root_sectors;
    u64 const entries_per_sector_scaled = bits == 32 ? (256ull * sectors_per_cluster + 2) / 2 : bits == 16 ? 256ull * sectors_per_cluster + 2 : (256ull * sectors_per_cluster * 4 / 3) + 2;
    u64 fat_sectors = (left + entries_per_sector_scaled - 1) / entries_per_sector_scaled;
    u64 const clusters = (total - reserved - root_sectors - 2 * fat_sectors) / sectors_per_cluster;
    if ((bits == 12 && clusters >= 4085) || (bits == 16 && (clusters < 4085 || clusters >= 65525)) || (bits == 32 && clusters < 65525))
        return Error::from_errno(EINVAL);
    // Enough for that many, however the arithmetic came out.
    u64 const fat_bytes = bits == 12 ? (clusters + 2) * 3 / 2 + 1 : (clusters + 2) * (bits / 8);
    fat_sectors = max<u64>(fat_sectors, (fat_bytes + Store::sector_size - 1) / Store::sector_size);

    char label_text[11];
    __builtin_memset(label_text, ' ', 11);
    size_t const label_length = min<size_t>(label.length(), 11);
    for (size_t i = 0; i < label_length; ++i) {
        char c = label[i];
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 32);
        label_text[i] = c;
    }
    bool const has_label = label_length > 0;
    i64 const time = clock ? clock() : 0;
    u32 const serial = static_cast<u32>(time) ^ 0x56554c54;

    SectorCache cache(store);
    TRY(cache.zero(0, min<u64>(cache.size(), (reserved + 2 * fat_sectors + root_sectors + (bits == 32 ? sectors_per_cluster : 0)) * Store::sector_size)));
    u8 boot[512] {};
    BootSector head {};
    head.jump[0] = 0xeb;
    head.jump[1] = bits == 32 ? 0x58 : 0x3c;
    head.jump[2] = 0x90;
    __builtin_memcpy(head.oem, "BSD  4.4", 8);
    head.bytes_per_sector = 512;
    head.sectors_per_cluster = static_cast<u8>(sectors_per_cluster);
    head.reserved_sectors = static_cast<u16>(reserved);
    head.fat_count = 2;
    head.root_entry_count = static_cast<u16>(root_entries);
    head.total_sectors_16 = total < 0x10000 && bits != 32 ? static_cast<u16>(total) : 0;
    head.media = 0xf8;
    head.fat_size_16 = bits == 32 ? 0 : static_cast<u16>(fat_sectors);
    head.sectors_per_track = 32;
    head.heads = 255;
    head.total_sectors_32 = head.total_sectors_16 ? 0 : static_cast<u32>(total);
    __builtin_memcpy(boot, &head, sizeof(head));
    Extended16 rest {};
    rest.drive = 0x80;
    rest.signature = 0x29;
    rest.serial = serial;
    __builtin_memcpy(rest.label, has_label ? label_text : "NO NAME    ", 11);
    __builtin_memcpy(rest.type, bits == 12 ? "FAT12   " : bits == 16 ? "FAT16   " : "FAT32   ", 8);
    if (bits == 32) {
        Extended32 extended {};
        extended.fat_size_32 = static_cast<u32>(fat_sectors);
        extended.root_cluster = 2;
        extended.info_sector = 1;
        extended.backup_boot_sector = 6;
        extended.rest = rest;
        __builtin_memcpy(boot + sizeof(head), &extended, sizeof(extended));
    } else {
        __builtin_memcpy(boot + sizeof(head), &rest, sizeof(rest));
    }
    boot[510] = 0x55;
    boot[511] = 0xaa;
    TRY(cache.write(0, { boot, sizeof(boot) }));
    if (bits == 32) {
        TRY(cache.write(6 * Store::sector_size, { boot, sizeof(boot) }));
        u8 info[512] {};
        u32 const free_count = static_cast<u32>(clusters - 1);
        u32 const next = 3;
        __builtin_memcpy(info, &fat32_info_lead, 4);
        __builtin_memcpy(info + 484, &fat32_info_struct, 4);
        __builtin_memcpy(info + 488, &free_count, 4);
        __builtin_memcpy(info + 492, &next, 4);
        __builtin_memcpy(info + 508, &fat32_info_trail, 4);
        TRY(cache.write(1 * Store::sector_size, { info, sizeof(info) }));
        TRY(cache.write(7 * Store::sector_size, { info, sizeof(info) }));
    }
    // The first two entries of each FAT; for FAT32, the root's cluster too.
    for (u32 copy = 0; copy < 2; ++copy) {
        u64 const start = (reserved + copy * fat_sectors) * Store::sector_size;
        if (bits == 12) {
            u8 const first[3] = { 0xf8, 0xff, 0xff };
            TRY(cache.write(start, { first, 3 }));
        } else if (bits == 16) {
            u8 const first[4] = { 0xf8, 0xff, 0xff, 0xff };
            TRY(cache.write(start, { first, 4 }));
        } else {
            u8 const first[12] = { 0xf8, 0xff, 0xff, 0x0f, 0xff, 0xff, 0xff, 0x0f, 0xff, 0xff, 0xff, 0x0f };
            TRY(cache.write(start, { first, 12 }));
        }
    }
    if (has_label) {
        DirectoryEntry entry {};
        __builtin_memcpy(entry.name, label_text, 11);
        entry.attributes = attribute_volume;
        entry.modification_date = dos_date_of(time);
        entry.modification_time = dos_time_of(time);
        u64 const root = (reserved + 2 * fat_sectors) * Store::sector_size;
        TRY(cache.write(root, { reinterpret_cast<u8 const*>(&entry), sizeof(entry) }));
    }
    return cache.flush();
}

}
