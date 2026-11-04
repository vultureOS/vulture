/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/HashMap.h>
#include <LibBase/New.h>
#include <LibForeignFS/SectorCache.h>
#include <LibForeignFS/Volume.h>
#include <LibUnicode/Unicode.h>

// exFAT, as Microsoft published it in 2019: "exFAT file system specification".

namespace ForeignFS {

namespace {

struct [[gnu::packed]] BootSector {
    u8 jump[3];
    char name[8];
    u8 zero[53];
    u64 partition_offset;
    u64 volume_length;
    u32 fat_offset;
    u32 fat_length;
    u32 heap_offset;
    u32 cluster_count;
    u32 root_cluster;
    u32 serial;
    u16 revision;
    u16 flags;
    u8 bytes_per_sector_shift;
    u8 sectors_per_cluster_shift;
    u8 fat_count;
    u8 drive;
    u8 percent_in_use;
    u8 reserved[7];
};
static_assert(sizeof(BootSector) == 120);

constexpr u16 flag_volume_dirty = 0x0002;

constexpr size_t entry_size = 32;
constexpr u8 type_end = 0x00;
constexpr u8 type_bitmap = 0x81;
constexpr u8 type_upcase = 0x82;
constexpr u8 type_label = 0x83;
constexpr u8 type_file = 0x85;
constexpr u8 type_stream = 0xc0;
constexpr u8 type_file_name = 0xc1;
constexpr u8 in_use_bit = 0x80;

constexpr u32 fat_end = 0xffffffff;

constexpr u16 attribute_read_only = 0x01;
constexpr u16 attribute_hidden = 0x02;
constexpr u16 attribute_directory = 0x10;
constexpr u16 attribute_archive = 0x20;

constexpr u8 stream_allocation_possible = 0x01;
constexpr u8 stream_no_fat_chain = 0x02;

struct [[gnu::packed]] FileEntry {
    u8 type;
    u8 secondary_count;
    u16 set_checksum;
    u16 attributes;
    u16 reserved;
    u32 created;
    u32 modified;
    u32 accessed;
    u8 created_10ms;
    u8 modified_10ms;
    u8 created_utc;
    u8 modified_utc;
    u8 accessed_utc;
    u8 reserved2[7];
};

struct [[gnu::packed]] StreamEntry {
    u8 type;
    u8 flags;
    u8 reserved;
    u8 name_length;
    u16 name_hash;
    u16 reserved2;
    u64 valid_length;
    u32 reserved3;
    u32 first_cluster;
    u64 length;
};

struct [[gnu::packed]] NameEntry {
    u8 type;
    u8 flags;
    u16 name[15];
};

struct [[gnu::packed]] LabelEntry {
    u8 type;
    u8 length;
    u16 text[11];
    u8 reserved[8];
};

struct [[gnu::packed]] BitmapEntry {
    u8 type;
    u8 flags;
    u8 reserved[18];
    u32 first_cluster;
    u64 length;
};

struct [[gnu::packed]] UpcaseEntry {
    u8 type;
    u8 reserved[3];
    u32 checksum;
    u8 reserved2[12];
    u32 first_cluster;
    u64 length;
};
static_assert(sizeof(FileEntry) == 32 && sizeof(StreamEntry) == 32 && sizeof(NameEntry) == 32 && sizeof(LabelEntry) == 32 && sizeof(BitmapEntry) == 32 && sizeof(UpcaseEntry) == 32);

// Howard Hinnant's method, as for FAT.
i64 days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    i64 const era = (year >= 0 ? year : year - 399) / 400;
    i64 const year_of_era = year - era * 400;
    i64 const day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    i64 const day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

u32 timestamp_of(i64 seconds)
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
    int const day = static_cast<int>(day_of_year - (153 * shifted_month + 2) / 5 + 1);
    i64 const year = year_of_era + era * 400 + (month <= 2);
    if (year < 1980)
        return (1 << 21) | (1 << 16);
    u32 const hour = static_cast<u32>(rest / 3600);
    u32 const minute = static_cast<u32>(rest / 60 % 60);
    u32 const second = static_cast<u32>(rest % 60);
    return (static_cast<u32>(year - 1980) << 25) | (static_cast<u32>(month) << 21) | (static_cast<u32>(day) << 16) | (hour << 11) | (minute << 5) | (second / 2);
}

i64 seconds_of(u32 stamp, u8 utc_offset)
{
    if (stamp == 0)
        return 0;
    u32 const date = stamp >> 16;
    u32 const time = stamp & 0xffff;
    i64 seconds = days_from_civil(1980 + (date >> 9), max<u32>((date >> 5) & 15, 1), max<u32>(date & 31, 1)) * 86400;
    seconds += (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
    // In units of 15 minutes from UTC, when the top bit says it's there.
    if (utc_offset & 0x80) {
        int offset = utc_offset & 0x7f;
        if (offset >= 64)
            offset -= 128;
        seconds -= offset * 15 * 60;
    }
    return seconds;
}

u32 boot_checksum(u8 const* bytes, size_t sector_size)
{
    u32 checksum = 0;
    for (size_t i = 0; i < sector_size * 11; ++i) {
        // The flags and how full it is can change without this having to.
        if (i == 106 || i == 107 || i == 112)
            continue;
        checksum = ((checksum & 1) ? 0x80000000u : 0) + (checksum >> 1) + bytes[i];
    }
    return checksum;
}

u32 upcase_checksum(u8 const* bytes, size_t size)
{
    u32 checksum = 0;
    for (size_t i = 0; i < size; ++i)
        checksum = ((checksum & 1) ? 0x80000000u : 0) + (checksum >> 1) + bytes[i];
    return checksum;
}

u16 set_checksum_of(u8 const* entries, size_t count)
{
    u16 checksum = 0;
    for (size_t i = 0; i < count * entry_size; ++i) {
        if (i == 2 || i == 3)
            continue;
        checksum = static_cast<u16>(((checksum & 1) ? 0x8000 : 0) + (checksum >> 1) + entries[i]);
    }
    return checksum;
}

class ExFATVolume final : public Volume {
public:
    ExFATVolume(Store& store, Clock clock, bool read_only)
        : m_cache(store)
        , m_clock(clock)
        , m_read_only(read_only || store.is_read_only())
    {
    }

    ~ExFATVolume() override
    {
        m_nodes.for_each([](NodeIndex, Node*& node) { delete node; });
    }

    ErrorOr<void> open();

    [[nodiscard]] StringView type_name() const override { return "exfat"_sv; }
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
    ErrorOr<void> repair(Function<void(StringView)>) override;

    ErrorOr<CheckReport> check();

private:
    static constexpr NodeIndex root_index = 1;

    struct Node {
        NodeIndex parent { 0 };
        u64 entry_offset { 0 };
        u32 secondary_count { 0 };
        u32 first_cluster { 0 };
        u64 length { 0 };
        u64 valid_length { 0 };
        u8 stream_flags { stream_allocation_possible };
        u16 attributes { 0 };
        i64 created { 0 };
        i64 modified { 0 };
        i64 accessed { 0 };
        bool removed { false };
        bool is_directory { false };
        u64 last_position { 0 };
        u32 last_cluster { 0 };
    };

    struct Found {
        Node node;
        String name;
    };

    using EntryVisitor = Function<ErrorOr<Iteration>(Found&)>;

    [[nodiscard]] u64 byte_of_cluster(u32 cluster) const
    {
        return m_heap_start + static_cast<u64>(cluster - 2) * m_cluster_size;
    }
    [[nodiscard]] bool is_valid_cluster(u32 cluster) const { return cluster >= 2 && cluster - 2 < m_cluster_count; }

    ErrorOr<u32> get_fat(u32 cluster);
    ErrorOr<void> set_fat(u32 cluster, u32 value);
    ErrorOr<bool> is_allocated(u32 cluster);
    ErrorOr<void> set_allocated(u32 cluster, bool);
    ErrorOr<u32> allocate_cluster();
    ErrorOr<void> free_clusters(Node&);
    ErrorOr<void> load_free_count();

    ErrorOr<Node*> node_of(NodeIndex);
    [[nodiscard]] NodeInfo info_of(NodeIndex, Node const&) const;
    ErrorOr<NodeIndex> index_of(NodeIndex parent, Found const&);

    ErrorOr<Optional<u32>> cluster_at(Node&, u64 position, bool extend);
    // A chain that was all in a row, and now won't be: its entries in the FAT, which it didn't need before.
    ErrorOr<void> make_chain_explicit(Node&);
    ErrorOr<size_t> read_stream(Node&, u64 offset, Bytes);
    ErrorOr<void> write_stream(Node&, u64 offset, ReadonlyBytes);

    ErrorOr<void> walk_directory(Node&, EntryVisitor);
    ErrorOr<Optional<Found>> find(Node& directory, StringView name);
    ErrorOr<void> store_entries(Node&);
    ErrorOr<void> remove_entries(Node& directory, u64 offset, u32 secondary_count);
    ErrorOr<void> add_entries(Node& directory, Vector<u16> const& name, Node& node);
    ErrorOr<u16> name_hash(Span<u16 const>);
    ErrorOr<void> shrink(Node&, u64 size);
    ErrorOr<void> mark_changed();
    ErrorOr<void> set_dirty(bool);
    [[nodiscard]] i64 now() const { return m_clock ? m_clock() : 0; }

    SectorCache m_cache;
    Clock m_clock;
    bool m_read_only;
    bool m_changed { false };
    String m_label;
    u32 m_sector_size { 512 };
    u32 m_cluster_size { 0 };
    u32 m_cluster_count { 0 };
    u64 m_fat_start { 0 };
    u64 m_heap_start { 0 };
    u32 m_bitmap_cluster { 0 };
    u64 m_bitmap_start { 0 };
    u32 m_upcase_cluster { 0 };
    u64 m_upcase_length { 0 };
    u32 m_upcase_checksum { 0 };
    u32 m_next_free { 2 };
    Optional<u32> m_free_clusters;
    // By pointer: a node stays where it is while others come and go.
    HashMap<NodeIndex, Node*> m_nodes;
    HashMap<u64, NodeIndex> m_by_location;
    NodeIndex m_next_index { 2 };
    u16 m_flags { 0 };
    // What's upper case of what: all of the 16 bits.
    Vector<u16> m_upcase;
};

ErrorOr<void> ExFATVolume::open()
{
    u8 first[512];
    TRY(m_cache.read(0, { first, sizeof(first) }));
    BootSector boot;
    __builtin_memcpy(&boot, first, sizeof(boot));
    if (__builtin_memcmp(boot.name, "EXFAT   ", 8) != 0 || first[510] != 0x55 || first[511] != 0xaa)
        return Error::from_errno(EINVAL);
    if (boot.bytes_per_sector_shift < 9 || boot.bytes_per_sector_shift > 12 || boot.fat_count == 0)
        return Error::from_errno(EINVAL);
    m_sector_size = 1u << boot.bytes_per_sector_shift;
    m_cluster_size = m_sector_size << boot.sectors_per_cluster_shift;
    m_cluster_count = boot.cluster_count;
    m_fat_start = static_cast<u64>(boot.fat_offset) * m_sector_size;
    m_heap_start = static_cast<u64>(boot.heap_offset) * m_sector_size;
    m_flags = boot.flags;
    if (m_heap_start + static_cast<u64>(m_cluster_count) * m_cluster_size > m_cache.size())
        return Error::from_errno(EINVAL);

    Node root;
    root.is_directory = true;
    root.attributes = attribute_directory;
    root.first_cluster = boot.root_cluster;
    root.stream_flags = stream_allocation_possible;
    auto* root_copy = new (Base::nothrow) Node(root);
    if (!root_copy)
        return Error::from_errno(ENOMEM);
    TRY(m_nodes.try_set(root_index, root_copy));

    auto& root_node = *TRY(node_of(root_index));
    root_node.length = 0xffffffffffffull;
    for (u64 offset = 0;; offset += entry_size) {
        u8 bytes[entry_size];
        if (TRY(read_stream(root_node, offset, { bytes, entry_size })) != entry_size || bytes[0] == type_end)
            break;
        if (bytes[0] == type_bitmap) {
            BitmapEntry entry;
            __builtin_memcpy(&entry, bytes, sizeof(entry));
            m_bitmap_cluster = entry.first_cluster;
            m_bitmap_start = byte_of_cluster(entry.first_cluster);
        } else if (bytes[0] == type_upcase) {
            UpcaseEntry entry;
            __builtin_memcpy(&entry, bytes, sizeof(entry));
            m_upcase_cluster = entry.first_cluster;
            m_upcase_length = entry.length;
            m_upcase_checksum = entry.checksum;
        } else if (bytes[0] == type_label) {
            LabelEntry entry;
            __builtin_memcpy(&entry, bytes, sizeof(entry));
            u16 text[11];
            __builtin_memcpy(text, entry.text, sizeof(text));
            m_label = TRY(utf8_of_utf16({ text, min<size_t>(entry.length, 11) }));
        }
    }
    if (m_bitmap_cluster == 0 || m_upcase_cluster == 0)
        return Error::from_errno(EINVAL);

    // How long the root really is, by its chain.
    u64 length = 0;
    u32 cluster = root_node.first_cluster;
    for (u32 guard = 0; is_valid_cluster(cluster) && guard <= m_cluster_count; ++guard) {
        length += m_cluster_size;
        cluster = TRY(get_fat(cluster));
        if (cluster == fat_end)
            break;
    }
    root_node.length = length;
    root_node.valid_length = length;
    root_node.last_cluster = 0;
    root_node.last_position = 0;

    // Up case: a run of 0xffff, then how many of what follows are themselves.
    TRY(m_upcase.try_resize(65536));
    for (size_t i = 0; i < 65536; ++i)
        m_upcase[i] = static_cast<u16>(i);
    Vector<u8> table;
    TRY(table.try_resize(m_upcase_length));
    u64 const table_start = byte_of_cluster(m_upcase_cluster);
    TRY(m_cache.read(table_start, { table.begin(), table.size() }));
    size_t at = 0;
    for (size_t i = 0; i + 1 < table.size() && at < 65536; i += 2) {
        u16 const value = static_cast<u16>(table[i] | (table[i + 1] << 8));
        if (value == 0xffff && i + 3 < table.size()) {
            at += static_cast<u16>(table[i + 2] | (table[i + 3] << 8));
            i += 2;
        } else {
            m_upcase[at++] = value;
        }
    }
    return {};
}

ErrorOr<void> ExFATVolume::load_free_count()
{
    if (m_free_clusters.has_value())
        return {};
    u32 count = 0;
    u8 buffer[SectorCache::piece_size];
    for (u32 at = 0; at < m_cluster_count; at += SectorCache::piece_size * 8) {
        u32 const clusters = min<u32>(m_cluster_count - at, SectorCache::piece_size * 8);
        size_t const bytes = (clusters + 7) / 8;
        TRY(m_cache.read(m_bitmap_start + at / 8, { buffer, bytes }));
        for (u32 i = 0; i < clusters; ++i) {
            if (!(buffer[i / 8] & (1 << (i % 8))))
                ++count;
        }
    }
    m_free_clusters = count;
    return {};
}

ErrorOr<Statistics> ExFATVolume::statistics()
{
    TRY(load_free_count());
    return Statistics { m_cluster_size, m_cluster_count, m_free_clusters.value() };
}

ErrorOr<u32> ExFATVolume::get_fat(u32 cluster)
{
    u32 value;
    TRY(m_cache.read(m_fat_start + cluster * 4ull, { reinterpret_cast<u8*>(&value), 4 }));
    return value;
}

ErrorOr<void> ExFATVolume::set_fat(u32 cluster, u32 value)
{
    return m_cache.write(m_fat_start + cluster * 4ull, { reinterpret_cast<u8 const*>(&value), 4 });
}

ErrorOr<bool> ExFATVolume::is_allocated(u32 cluster)
{
    u32 const index = cluster - 2;
    u8 byte;
    TRY(m_cache.read(m_bitmap_start + index / 8, { &byte, 1 }));
    return byte & (1 << (index % 8));
}

ErrorOr<void> ExFATVolume::set_allocated(u32 cluster, bool allocated)
{
    u32 const index = cluster - 2;
    u8 byte;
    TRY(m_cache.read(m_bitmap_start + index / 8, { &byte, 1 }));
    byte = allocated ? byte | (1 << (index % 8)) : byte & ~(1 << (index % 8));
    return m_cache.write(m_bitmap_start + index / 8, { &byte, 1 });
}

ErrorOr<u32> ExFATVolume::allocate_cluster()
{
    TRY(load_free_count());
    if (m_free_clusters.value() == 0)
        return Error::from_errno(ENOSPC);
    u32 cluster = is_valid_cluster(m_next_free) ? m_next_free : 2;
    for (u32 tried = 0; tried < m_cluster_count; ++tried) {
        if (!TRY(is_allocated(cluster))) {
            TRY(set_allocated(cluster, true));
            TRY(set_fat(cluster, fat_end));
            m_free_clusters = m_free_clusters.value() - 1;
            m_next_free = cluster + 1 < m_cluster_count + 2 ? cluster + 1 : 2;
            return cluster;
        }
        ++cluster;
        if (cluster >= m_cluster_count + 2)
            cluster = 2;
    }
    return Error::from_errno(ENOSPC);
}

ErrorOr<ExFATVolume::Node*> ExFATVolume::node_of(NodeIndex index)
{
    auto* node = m_nodes.get(index);
    if (!node)
        return Error::from_errno(ENOENT);
    return *node;
}

NodeInfo ExFATVolume::info_of(NodeIndex index, Node const& node) const
{
    NodeInfo info;
    info.index = index;
    info.size = node.length;
    info.allocated = (node.length + m_cluster_size - 1) / m_cluster_size * m_cluster_size;
    info.is_directory = node.is_directory;
    info.read_only = node.attributes & attribute_read_only;
    info.hidden = node.attributes & attribute_hidden;
    info.created = node.created;
    info.modified = node.modified;
    info.accessed = node.accessed;
    return info;
}

ErrorOr<NodeIndex> ExFATVolume::index_of(NodeIndex parent, Found const& found)
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

ErrorOr<NodeInfo> ExFATVolume::info(NodeIndex index)
{
    return info_of(index, *TRY(node_of(index)));
}

ErrorOr<void> ExFATVolume::make_chain_explicit(Node& node)
{
    if (!(node.stream_flags & stream_no_fat_chain) || node.first_cluster == 0)
        return {};
    u64 const clusters = (node.length + m_cluster_size - 1) / m_cluster_size;
    for (u64 i = 0; i + 1 < clusters; ++i)
        TRY(set_fat(static_cast<u32>(node.first_cluster + i), static_cast<u32>(node.first_cluster + i + 1)));
    if (clusters)
        TRY(set_fat(static_cast<u32>(node.first_cluster + clusters - 1), fat_end));
    node.stream_flags &= ~stream_no_fat_chain;
    return {};
}

ErrorOr<Optional<u32>> ExFATVolume::cluster_at(Node& node, u64 position, bool extend)
{
    u64 const wanted = position / m_cluster_size;
    if (node.stream_flags & stream_no_fat_chain) {
        u64 const have = (node.length + m_cluster_size - 1) / m_cluster_size;
        if (wanted < have)
            return Optional<u32>(static_cast<u32>(node.first_cluster + wanted));
        if (!extend)
            return Optional<u32> {};
        TRY(make_chain_explicit(node));
    }
    u32 cluster = node.first_cluster;
    u64 here = 0;
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
        if (next == fat_end || next == 0) {
            if (!extend)
                return Optional<u32> {};
            next = TRY(allocate_cluster());
            TRY(set_fat(cluster, next));
            if (node.is_directory)
                TRY(m_cache.zero(byte_of_cluster(next), m_cluster_size));
        } else if (!is_valid_cluster(next)) {
            return Error::from_errno(EIO);
        }
        cluster = next;
        ++here;
    }
    node.last_position = here;
    node.last_cluster = cluster;
    return Optional<u32>(cluster);
}

ErrorOr<size_t> ExFATVolume::read_stream(Node& node, u64 offset, Bytes buffer)
{
    size_t done = 0;
    while (done < buffer.size()) {
        u64 const position = offset + done;
        auto const cluster = TRY(cluster_at(node, position, false));
        if (!cluster.has_value())
            break;
        size_t const inside = position % m_cluster_size;
        size_t const part = min<size_t>(buffer.size() - done, m_cluster_size - inside);
        TRY(m_cache.read(byte_of_cluster(cluster.value()) + inside, { buffer.data() + done, part }));
        done += part;
    }
    return done;
}

ErrorOr<void> ExFATVolume::write_stream(Node& node, u64 offset, ReadonlyBytes buffer)
{
    size_t done = 0;
    while (done < buffer.size()) {
        u64 const position = offset + done;
        auto const cluster = TRY(cluster_at(node, position, true));
        size_t const inside = position % m_cluster_size;
        size_t const part = min<size_t>(buffer.size() - done, m_cluster_size - inside);
        TRY(m_cache.write(byte_of_cluster(cluster.value()) + inside, { buffer.data() + done, part }));
        done += part;
    }
    return {};
}

ErrorOr<void> ExFATVolume::walk_directory(Node& directory, EntryVisitor visitor)
{
    u64 const capacity = directory.length;
    for (u64 offset = 0; offset < capacity; offset += entry_size) {
        u8 bytes[entry_size];
        if (TRY(read_stream(directory, offset, { bytes, entry_size })) != entry_size)
            break;
        if (bytes[0] == type_end)
            break;
        if (bytes[0] != type_file)
            continue;
        FileEntry file;
        __builtin_memcpy(&file, bytes, sizeof(file));
        if (file.secondary_count < 2 || offset + (file.secondary_count + 1ull) * entry_size > capacity)
            continue;
        u8 stream_bytes[entry_size];
        TRY(read_stream(directory, offset + entry_size, { stream_bytes, entry_size }));
        if (stream_bytes[0] != type_stream)
            continue;
        StreamEntry stream;
        __builtin_memcpy(&stream, stream_bytes, sizeof(stream));
        Vector<u16> name;
        for (u32 i = 2; i <= file.secondary_count && name.size() < stream.name_length; ++i) {
            u8 name_bytes[entry_size];
            TRY(read_stream(directory, offset + i * entry_size, { name_bytes, entry_size }));
            if (name_bytes[0] != type_file_name)
                break;
            NameEntry entry;
            __builtin_memcpy(&entry, name_bytes, sizeof(entry));
            u16 units[15];
            __builtin_memcpy(units, entry.name, sizeof(units));
            for (size_t j = 0; j < 15 && name.size() < stream.name_length; ++j)
                TRY(name.try_append(units[j]));
        }
        Found found;
        found.name = TRY(utf8_of_utf16({ name.begin(), name.size() }));
        found.node.entry_offset = offset;
        found.node.secondary_count = file.secondary_count;
        found.node.first_cluster = stream.first_cluster;
        found.node.length = stream.length;
        found.node.valid_length = stream.valid_length;
        found.node.stream_flags = stream.flags;
        found.node.attributes = file.attributes;
        found.node.is_directory = file.attributes & attribute_directory;
        found.node.created = seconds_of(file.created, file.created_utc);
        found.node.modified = seconds_of(file.modified, file.modified_utc);
        found.node.accessed = seconds_of(file.accessed, file.accessed_utc);
        if (TRY(visitor(found)) == Iteration::Stop)
            break;
        offset += file.secondary_count * entry_size;
    }
    return {};
}

ErrorOr<Optional<ExFATVolume::Found>> ExFATVolume::find(Node& directory, StringView name)
{
    auto const wanted = TRY(comparable_name(name));
    Optional<Found> result;
    TRY(walk_directory(directory, [&](Found& found) -> ErrorOr<Iteration> {
        auto const here = TRY(comparable_name(found.name.view()));
        if (here.size() == wanted.size() && __builtin_memcmp(here.begin(), wanted.begin(), wanted.size() * sizeof(u16)) == 0) {
            result = move(found);
            return Iteration::Stop;
        }
        return Iteration::Continue;
    }));
    return result;
}

ErrorOr<NodeInfo> ExFATVolume::lookup(NodeIndex directory_index, StringView name)
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
    auto found = TRY(find(directory, name));
    if (!found.has_value())
        return Error::from_errno(ENOENT);
    auto const index = TRY(index_of(directory_index, found.value()));
    return info_of(index, *TRY(node_of(index)));
}

ErrorOr<void> ExFATVolume::for_each_entry(NodeIndex directory_index, Function<Iteration(StringView, NodeInfo const&)> callback)
{
    auto& directory = *TRY(node_of(directory_index));
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    return walk_directory(directory, [&](Found& found) -> ErrorOr<Iteration> {
        auto const index = TRY(index_of(directory_index, found));
        return callback(found.name.view(), info_of(index, *TRY(node_of(index))));
    });
}

ErrorOr<size_t> ExFATVolume::read(NodeIndex index, u64 offset, Bytes buffer)
{
    auto& node = *TRY(node_of(index));
    if (node.is_directory)
        return Error::from_errno(EISDIR);
    if (offset >= node.length)
        return 0;
    size_t const count = min<u64>(buffer.size(), node.length - offset);
    // What's past what has been written reads as zeros, without being on the disk as such.
    if (offset >= node.valid_length) {
        __builtin_memset(buffer.data(), 0, count);
        return count;
    }
    size_t const stored = min<u64>(count, node.valid_length - offset);
    size_t const got = TRY(read_stream(node, offset, buffer.trim(stored)));
    if (got < stored)
        return got;
    if (count > stored)
        __builtin_memset(buffer.data() + stored, 0, count - stored);
    return count;
}

ErrorOr<void> ExFATVolume::mark_changed()
{
    if (m_read_only)
        return Error::from_errno(EROFS);
    if (!m_changed) {
        m_changed = true;
        TRY(set_dirty(true));
    }
    return {};
}

ErrorOr<void> ExFATVolume::set_dirty(bool dirty)
{
    u16 const flags = dirty ? m_flags | flag_volume_dirty : m_flags & ~flag_volume_dirty;
    TRY(m_cache.write(106, { reinterpret_cast<u8 const*>(&flags), 2 }));
    m_flags = flags;
    return {};
}

ErrorOr<size_t> ExFATVolume::write(NodeIndex index, u64 offset, ReadonlyBytes buffer)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    if (node.is_directory)
        return Error::from_errno(EISDIR);
    if (node.attributes & attribute_read_only)
        return Error::from_errno(EACCES);
    if (buffer.is_empty())
        return 0;
    if (offset > node.length)
        TRY(truncate(index, offset));
    TRY(write_stream(node, offset, buffer));
    u64 const end = offset + buffer.size();
    if (end > node.length)
        node.length = end;
    if (end > node.valid_length) {
        // What was between the two has to read as zeros now it's before what's written.
        if (offset > node.valid_length) {
            u64 position = node.valid_length;
            while (position < offset) {
                auto const cluster = TRY(cluster_at(node, position, true));
                size_t const inside = position % m_cluster_size;
                size_t const part = min<u64>(offset - position, m_cluster_size - inside);
                TRY(m_cache.zero(byte_of_cluster(cluster.value()) + inside, part));
                position += part;
            }
        }
        node.valid_length = end;
    }
    node.modified = now();
    node.accessed = node.modified;
    node.attributes |= attribute_archive;
    TRY(store_entries(node));
    return buffer.size();
}

ErrorOr<void> ExFATVolume::free_clusters(Node& node)
{
    TRY(load_free_count());
    if (node.first_cluster == 0)
        return {};
    u32 count = m_free_clusters.value();
    if (node.stream_flags & stream_no_fat_chain) {
        u64 const clusters = (node.length + m_cluster_size - 1) / m_cluster_size;
        for (u64 i = 0; i < clusters; ++i) {
            TRY(set_allocated(static_cast<u32>(node.first_cluster + i), false));
            ++count;
        }
    } else {
        u32 cluster = node.first_cluster;
        for (u32 guard = 0; is_valid_cluster(cluster) && guard <= m_cluster_count; ++guard) {
            u32 const next = TRY(get_fat(cluster));
            TRY(set_allocated(cluster, false));
            TRY(set_fat(cluster, 0));
            ++count;
            if (next == fat_end || next == 0)
                break;
            cluster = next;
        }
    }
    m_free_clusters = count;
    node.first_cluster = 0;
    return {};
}

ErrorOr<void> ExFATVolume::shrink(Node& node, u64 size)
{
    if (size == 0) {
        TRY(free_clusters(node));
        node.stream_flags = stream_allocation_possible;
    } else {
        TRY(make_chain_explicit(node));
        auto const last = TRY(cluster_at(node, size - 1, false));
        if (!last.has_value())
            return Error::from_errno(EIO);
        auto next = TRY(get_fat(last.value()));
        TRY(set_fat(last.value(), fat_end));
        TRY(load_free_count());
        u32 count = m_free_clusters.value();
        for (u32 guard = 0; is_valid_cluster(next) && guard <= m_cluster_count; ++guard) {
            u32 const after = TRY(get_fat(next));
            TRY(set_allocated(next, false));
            TRY(set_fat(next, 0));
            ++count;
            if (after == fat_end || after == 0)
                break;
            next = after;
        }
        m_free_clusters = count;
    }
    node.last_cluster = 0;
    node.last_position = 0;
    return {};
}

ErrorOr<void> ExFATVolume::truncate(NodeIndex index, u64 size)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    if (node.is_directory)
        return Error::from_errno(EISDIR);
    if (size == node.length)
        return {};
    if (size < node.length) {
        TRY(shrink(node, size));
        node.length = size;
        node.valid_length = min(node.valid_length, size);
    } else {
        // Room for it: with valid_length as it was, what's new reads as zeros without being written.
        u64 position = (node.length + m_cluster_size - 1) / m_cluster_size * m_cluster_size;
        while (position < size) {
            (void)TRY(cluster_at(node, position, true));
            position += m_cluster_size;
        }
        node.length = size;
    }
    node.modified = now();
    node.attributes |= attribute_archive;
    return store_entries(node);
}

ErrorOr<void> ExFATVolume::store_entries(Node& node)
{
    if (node.parent == 0) {
        // The root has no entry; its chain is all there is to say.
        return {};
    }
    if (node.removed)
        return {};
    auto& parent = *TRY(node_of(node.parent));
    size_t const total = (node.secondary_count + 1) * entry_size;
    Vector<u8> bytes;
    TRY(bytes.try_resize(total));
    if (TRY(read_stream(parent, node.entry_offset, { bytes.begin(), total })) != total)
        return Error::from_errno(EIO);
    FileEntry file;
    __builtin_memcpy(&file, bytes.begin(), sizeof(file));
    file.attributes = node.attributes;
    file.modified = timestamp_of(node.modified);
    file.modified_10ms = 0;
    file.modified_utc = 0x80;
    file.accessed = timestamp_of(node.accessed);
    file.accessed_utc = 0x80;
    __builtin_memcpy(bytes.begin(), &file, sizeof(file));
    StreamEntry stream;
    __builtin_memcpy(&stream, bytes.begin() + entry_size, sizeof(stream));
    stream.flags = node.first_cluster == 0 ? stream_allocation_possible : node.stream_flags;
    stream.first_cluster = node.first_cluster;
    stream.length = node.length;
    stream.valid_length = node.valid_length;
    __builtin_memcpy(bytes.begin() + entry_size, &stream, sizeof(stream));
    u16 const checksum = set_checksum_of(bytes.begin(), node.secondary_count + 1);
    __builtin_memcpy(bytes.begin() + 2, &checksum, 2);
    return write_stream(parent, node.entry_offset, { bytes.begin(), total });
}

ErrorOr<u16> ExFATVolume::name_hash(Span<u16 const> name)
{
    u16 hash = 0;
    for (auto unit : name) {
        u16 const upper = m_upcase[unit];
        hash = static_cast<u16>(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (upper & 0xff));
        hash = static_cast<u16>(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (upper >> 8));
    }
    return hash;
}

ErrorOr<void> ExFATVolume::add_entries(Node& directory, Vector<u16> const& name, Node& node)
{
    u32 const name_entries = static_cast<u32>((name.size() + 14) / 15);
    u32 const wanted = 2 + name_entries;

    // A run of entries nothing's using, or else the end. A directory is one or more whole clusters.
    u64 run_start = 0;
    u32 run = 0;
    u64 offset = 0;
    bool found_room = false;
    for (; offset < directory.length; offset += entry_size) {
        u8 first;
        TRY(read_stream(directory, offset, { &first, 1 }));
        if (first == type_end) {
            if (run == 0)
                run_start = offset;
            found_room = true;
            break;
        }
        if (!(first & in_use_bit)) {
            if (run == 0)
                run_start = offset;
            if (++run == wanted) {
                found_room = true;
                break;
            }
        } else {
            run = 0;
        }
    }
    if (!found_room) {
        if (run == 0)
            run_start = offset;
    }
    u64 const end = run_start + wanted * entry_size;
    if (end > 256 * 1024 * 1024)
        return Error::from_errno(ENOSPC);
    if (end > directory.length) {
        // Grows by a cluster at a time, each one zero.
        u64 position = directory.length;
        while (position < end) {
            auto const cluster = TRY(cluster_at(directory, position, true));
            TRY(m_cache.zero(byte_of_cluster(cluster.value()), m_cluster_size));
            position += m_cluster_size;
        }
        directory.length = position;
        directory.valid_length = position;
        TRY(store_entries(directory));
    }

    i64 const time = now();
    Vector<u8> bytes;
    TRY(bytes.try_resize(wanted * entry_size));
    __builtin_memset(bytes.begin(), 0, bytes.size());
    FileEntry file {};
    file.type = type_file;
    file.secondary_count = static_cast<u8>(wanted - 1);
    file.attributes = node.attributes;
    file.created = timestamp_of(time);
    file.modified = file.created;
    file.accessed = file.created;
    file.created_utc = 0x80;
    file.modified_utc = 0x80;
    file.accessed_utc = 0x80;
    __builtin_memcpy(bytes.begin(), &file, sizeof(file));
    StreamEntry stream {};
    stream.type = type_stream;
    stream.flags = node.first_cluster == 0 ? stream_allocation_possible : node.stream_flags;
    stream.name_length = static_cast<u8>(name.size());
    stream.name_hash = TRY(name_hash({ name.begin(), name.size() }));
    stream.first_cluster = node.first_cluster;
    stream.length = node.length;
    stream.valid_length = node.valid_length;
    __builtin_memcpy(bytes.begin() + entry_size, &stream, sizeof(stream));
    for (u32 i = 0; i < name_entries; ++i) {
        NameEntry entry {};
        entry.type = type_file_name;
        u16 units[15] {};
        for (size_t j = 0; j < 15 && i * 15 + j < name.size(); ++j)
            units[j] = name[i * 15 + j];
        __builtin_memcpy(entry.name, units, sizeof(units));
        __builtin_memcpy(bytes.begin() + (2 + i) * entry_size, &entry, sizeof(entry));
    }
    u16 const checksum = set_checksum_of(bytes.begin(), wanted);
    __builtin_memcpy(bytes.begin() + 2, &checksum, 2);
    TRY(write_stream(directory, run_start, { bytes.begin(), bytes.size() }));

    node.entry_offset = run_start;
    node.secondary_count = wanted - 1;
    return {};
}

ErrorOr<NodeInfo> ExFATVolume::create(NodeIndex directory_index, StringView name, bool is_directory)
{
    TRY(mark_changed());
    auto& directory = *TRY(node_of(directory_index));
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    if (TRY(find(directory, name)).has_value())
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
    Node node;
    node.parent = directory_index;
    node.is_directory = is_directory;
    node.attributes = is_directory ? attribute_directory : attribute_archive;
    node.created = now();
    node.modified = node.created;
    node.accessed = node.created;
    if (is_directory) {
        node.first_cluster = TRY(allocate_cluster());
        TRY(m_cache.zero(byte_of_cluster(node.first_cluster), m_cluster_size));
        node.length = m_cluster_size;
        node.valid_length = m_cluster_size;
    }
    TRY(add_entries(directory, units, node));
    directory.modified = node.created;
    TRY(store_entries(directory));
    m_by_location.remove((directory_index << 32) ^ node.entry_offset);
    Found found;
    found.node = node;
    found.name = TRY(String::from_view(name));
    auto const index = TRY(index_of(directory_index, found));
    return info_of(index, *TRY(node_of(index)));
}

ErrorOr<void> ExFATVolume::remove_entries(Node& directory, u64 offset, u32 secondary_count)
{
    for (u32 i = 0; i <= secondary_count; ++i) {
        u8 type;
        TRY(read_stream(directory, offset + i * entry_size, { &type, 1 }));
        type &= ~in_use_bit;
        TRY(write_stream(directory, offset + i * entry_size, { &type, 1 }));
    }
    return {};
}

ErrorOr<void> ExFATVolume::remove(NodeIndex directory_index, StringView name)
{
    TRY(mark_changed());
    auto& directory = *TRY(node_of(directory_index));
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    auto found = TRY(find(directory, name));
    if (!found.has_value())
        return Error::from_errno(ENOENT);
    auto const index = TRY(index_of(directory_index, found.value()));
    auto& node = *TRY(node_of(index));
    if (node.is_directory) {
        bool empty = true;
        TRY(walk_directory(node, [&](Found&) -> ErrorOr<Iteration> {
            empty = false;
            return Iteration::Stop;
        }));
        if (!empty)
            return Error::from_errno(ENOTEMPTY);
    }
    if (node.attributes & attribute_read_only)
        return Error::from_errno(EACCES);
    TRY(remove_entries(directory, node.entry_offset, node.secondary_count));
    m_by_location.remove((directory_index << 32) ^ node.entry_offset);
    node.removed = true;
    directory.modified = now();
    return store_entries(directory);
}

ErrorOr<void> ExFATVolume::rename(NodeIndex from_directory_index, StringView from_name, NodeIndex to_directory_index, StringView to_name)
{
    TRY(mark_changed());
    auto& from_directory = *TRY(node_of(from_directory_index));
    auto& to_directory = *TRY(node_of(to_directory_index));
    if (!from_directory.is_directory || !to_directory.is_directory)
        return Error::from_errno(ENOTDIR);
    auto found = TRY(find(from_directory, from_name));
    if (!found.has_value())
        return Error::from_errno(ENOENT);
    auto const index = TRY(index_of(from_directory_index, found.value()));
    auto& moving = *TRY(node_of(index));
    if (moving.attributes & attribute_read_only)
        return Error::from_errno(EACCES);
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
    auto existing = TRY(find(to_directory, to_name));
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
    u64 const old_offset = moving.entry_offset;
    u32 const old_secondary = moving.secondary_count;
    TRY(remove_entries(from_directory, old_offset, old_secondary));
    m_by_location.remove((from_directory_index << 32) ^ old_offset);
    TRY(add_entries(to_directory, units, moving));
    moving.parent = to_directory_index;
    TRY(m_by_location.try_set((to_directory_index << 32) ^ moving.entry_offset, index));
    TRY(store_entries(moving));
    i64 const time = now();
    from_directory.modified = time;
    to_directory.modified = time;
    TRY(store_entries(from_directory));
    return store_entries(to_directory);
}

ErrorOr<void> ExFATVolume::set_times(NodeIndex index, i64 accessed, i64 modified)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    node.accessed = accessed;
    node.modified = modified;
    return store_entries(node);
}

ErrorOr<void> ExFATVolume::set_read_only(NodeIndex index, bool read_only)
{
    TRY(mark_changed());
    auto& node = *TRY(node_of(index));
    node.attributes = read_only ? node.attributes | attribute_read_only : node.attributes & ~attribute_read_only;
    return store_entries(node);
}

void ExFATVolume::forget(NodeIndex index)
{
    if (index == root_index)
        return;
    auto* found = m_nodes.get(index);
    if (!found)
        return;
    auto* node = *found;
    if (node->removed)
        (void)free_clusters(*node);
    else
        m_by_location.remove((node->parent << 32) ^ node->entry_offset);
    m_nodes.remove(index);
    delete node;
}

ErrorOr<void> ExFATVolume::flush()
{
    if (!m_changed)
        return {};
    TRY(load_free_count());
    u8 const percent = static_cast<u8>(m_cluster_count ? (m_cluster_count - m_free_clusters.value()) * 100ull / m_cluster_count : 0);
    TRY(m_cache.write(112, { &percent, 1 }));
    TRY(set_dirty(false));
    TRY(m_cache.flush());
    m_changed = false;
    return {};
}

ErrorOr<void> ExFATVolume::repair(Function<void(StringView)> say)
{
    if (m_read_only)
        return Error::from_errno(EROFS);
    m_changed = true;
    Vector<u8> seen;
    TRY(seen.try_resize(m_cluster_count + 2));
    __builtin_memset(seen.begin(), 0, seen.size());
    auto const tell = [&](StringView text) {
        if (say)
            say(text);
    };
    auto const mark = [&](u32 first, u64 length, bool contiguous) -> ErrorOr<void> {
        u64 const needed = (length + m_cluster_size - 1) / m_cluster_size;
        u32 cluster = first;
        for (u64 i = 0; i < needed && is_valid_cluster(cluster) && !seen[cluster]; ++i) {
            seen[cluster] = 1;
            cluster = contiguous ? cluster + 1 : TRY(get_fat(cluster));
        }
        return {};
    };
    TRY(mark(m_bitmap_cluster, (m_cluster_count + 7) / 8, false));
    TRY(mark(m_upcase_cluster, m_upcase_length, false));
    TRY(mark(TRY(node_of(root_index))->first_cluster, TRY(node_of(root_index))->length, false));
    Vector<NodeIndex> pending;
    TRY(pending.try_append(root_index));
    while (!pending.is_empty()) {
        auto const index = pending.take_last();
        auto& directory = *TRY(node_of(index));
        Vector<NodeIndex> inside;
        TRY(walk_directory(directory, [&](Found& found) -> ErrorOr<Iteration> {
            TRY(inside.try_append(TRY(index_of(index, found))));
            return Iteration::Continue;
        }));
        for (auto inner : inside) {
            auto& node = *TRY(node_of(inner));
            TRY(mark(node.first_cluster, node.length, node.stream_flags & stream_no_fat_chain));
            // Which also writes its checksum, whatever it was before.
            TRY(store_entries(node));
            if (node.is_directory)
                TRY(pending.try_append(inner));
        }
    }
    u32 lost = 0;
    u32 unmarked = 0;
    for (u32 cluster = 2; cluster < m_cluster_count + 2; ++cluster) {
        bool const allocated = TRY(is_allocated(cluster));
        if (allocated && !seen[cluster]) {
            TRY(set_allocated(cluster, false));
            ++lost;
        } else if (!allocated && seen[cluster]) {
            TRY(set_allocated(cluster, true));
            ++unmarked;
        }
    }
    if (lost)
        tell("Clusters that nothing was using are free again."_sv);
    if (unmarked)
        tell("Clusters in use that weren't marked as such now are."_sv);
    m_free_clusters = {};
    return flush();
}

ErrorOr<CheckReport> ExFATVolume::check()
{
    CheckReport report;
    auto const problem = [&](StringView text) -> ErrorOr<void> {
        return report.problems.try_append(TRY(String::from_view(text)));
    };
    Vector<u8> boot;
    TRY(boot.try_resize(m_sector_size * 12));
    TRY(m_cache.read(0, { boot.begin(), boot.size() }));
    u32 const expected = boot_checksum(boot.begin(), m_sector_size);
    u32 said;
    __builtin_memcpy(&said, boot.begin() + m_sector_size * 11, 4);
    if (expected != said)
        TRY(problem("the boot sectors' checksum is wrong"_sv));
    if (m_flags & flag_volume_dirty)
        TRY(problem("it wasn't put away in good order"_sv));

    Vector<u8> seen;
    TRY(seen.try_resize(m_cluster_count + 2));
    __builtin_memset(seen.begin(), 0, seen.size());
    auto const walk = [&](u32 first, u64 length, bool contiguous, StringView name) -> ErrorOr<void> {
        u64 const needed = (length + m_cluster_size - 1) / m_cluster_size;
        u32 cluster = first;
        for (u64 i = 0; i < needed; ++i) {
            if (!is_valid_cluster(cluster)) {
                StringBuilder text;
                text.appendff("{}: goes outside the volume", name);
                return problem(text.view());
            }
            if (seen[cluster]) {
                StringBuilder text;
                text.appendff("{}: cluster {} is used twice", name, cluster);
                return problem(text.view());
            }
            seen[cluster] = 1;
            if (!TRY(is_allocated(cluster))) {
                StringBuilder text;
                text.appendff("{}: cluster {} isn't marked as in use", name, cluster);
                TRY(problem(text.view()));
            }
            cluster = contiguous ? cluster + 1 : TRY(get_fat(cluster));
        }
        return {};
    };
    TRY(walk(m_bitmap_cluster, (m_cluster_count + 7) / 8, false, "the bitmap"_sv));
    TRY(walk(m_upcase_cluster, m_upcase_length, false, "the up-case table"_sv));
    TRY(walk(TRY(node_of(root_index))->first_cluster, TRY(node_of(root_index))->length, false, "the root"_sv));

    Vector<NodeIndex> pending;
    TRY(pending.try_append(root_index));
    while (!pending.is_empty()) {
        auto const index = pending.take_last();
        auto& directory = *TRY(node_of(index));
        Vector<NodeIndex> directories;
        TRY(walk_directory(directory, [&](Found& found) -> ErrorOr<Iteration> {
            auto const inner = TRY(index_of(index, found));
            // Each set of entries has its own checksum.
            Vector<u8> bytes;
            TRY(bytes.try_resize((found.node.secondary_count + 1) * entry_size));
            TRY(read_stream(directory, found.node.entry_offset, { bytes.begin(), bytes.size() }));
            u16 written;
            __builtin_memcpy(&written, bytes.begin() + 2, 2);
            if (written != set_checksum_of(bytes.begin(), found.node.secondary_count + 1)) {
                StringBuilder text;
                text.appendff("{}: its entries' checksum is wrong", found.name.view());
                TRY(problem(text.view()));
            }
            if (found.node.is_directory) {
                ++report.directories;
                TRY(directories.try_append(inner));
            } else {
                ++report.files;
            }
            TRY(walk(found.node.first_cluster, found.node.length, found.node.stream_flags & stream_no_fat_chain, found.name.view()));
            return Iteration::Continue;
        }));
        for (auto inner : directories)
            TRY(pending.try_append(inner));
    }
    u32 lost = 0;
    for (u32 cluster = 2; cluster < m_cluster_count + 2; ++cluster) {
        if (TRY(is_allocated(cluster)) && !seen[cluster])
            ++lost;
    }
    if (lost) {
        StringBuilder text;
        text.appendff("{} clusters are in use, but by nothing", lost);
        TRY(problem(text.view()));
    }
    return report;
}

}

ErrorOr<NonnullOwnPtr<Volume>> open_exfat(Store& store, Clock clock, bool read_only)
{
    auto* volume = new (Base::nothrow) ExFATVolume(store, clock, read_only);
    if (!volume)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*static_cast<Volume*>(volume));
    TRY(volume->open());
    return owned;
}

ErrorOr<CheckReport> check_exfat(Store& store, Clock clock)
{
    ExFATVolume volume(store, clock, true);
    TRY(volume.open());
    return volume.check();
}

ErrorOr<void> format_exfat(Store& store, StringView label, Clock clock)
{
    u64 const sectors = store.sector_count();
    if (sectors < 2048)
        return Error::from_errno(ENOSPC);
    // Microsoft's defaults: 4 KiB to 256 MiB, 32 KiB to 32 GiB, 128 KiB beyond that.
    u64 const bytes = sectors * Store::sector_size;
    u32 const cluster_shift = bytes <= 256 * 1024 * 1024 ? 3 : bytes <= 32ull * 1024 * 1024 * 1024 ? 6 : 8;
    u32 const sectors_per_cluster = 1u << cluster_shift;
    u32 const cluster_size = sectors_per_cluster * Store::sector_size;

    // The FAT starts after two copies of the boot region, and on a boundary of 1 MiB once there's room for that.
    u32 const alignment = sectors >= 2 * 1024 * 1024 / Store::sector_size * 32 ? 2048 : 128;
    u32 const fat_offset = 24 < alignment ? alignment : 24;
    u64 clusters = (sectors - fat_offset) / sectors_per_cluster;
    u32 fat_length = static_cast<u32>((((clusters + 2) * 4) + Store::sector_size - 1) / Store::sector_size);
    u32 const heap_offset = (fat_offset + fat_length + sectors_per_cluster - 1) / sectors_per_cluster * sectors_per_cluster;
    clusters = (sectors - heap_offset) / sectors_per_cluster;
    if (clusters < 8 || clusters > 0xfffffff5)
        return Error::from_errno(EINVAL);
    fat_length = static_cast<u32>((((clusters + 2) * 4) + Store::sector_size - 1) / Store::sector_size);

    i64 const time = clock ? clock() : 0;
    SectorCache cache(store);
    TRY(cache.zero(0, heap_offset * static_cast<u64>(Store::sector_size)));

    // ---- The boot region: the sector, eight extended ones, one of parameters, one reserved, and a checksum of those.
    Vector<u8> boot;
    TRY(boot.try_resize(Store::sector_size * 12));
    __builtin_memset(boot.begin(), 0, boot.size());
    BootSector head {};
    head.jump[0] = 0xeb;
    head.jump[1] = 0x76;
    head.jump[2] = 0x90;
    __builtin_memcpy(head.name, "EXFAT   ", 8);
    head.volume_length = sectors;
    head.fat_offset = fat_offset;
    head.fat_length = fat_length;
    head.heap_offset = heap_offset;
    head.cluster_count = static_cast<u32>(clusters);
    head.serial = static_cast<u32>(time) ^ 0x45584654;
    head.revision = 0x0100;
    head.bytes_per_sector_shift = 9;
    head.sectors_per_cluster_shift = static_cast<u8>(cluster_shift);
    head.fat_count = 1;
    head.drive = 0x80;
    __builtin_memcpy(boot.begin(), &head, sizeof(head));
    boot[510] = 0x55;
    boot[511] = 0xaa;
    for (size_t i = 1; i <= 8; ++i) {
        boot[i * Store::sector_size + 510] = 0x55;
        boot[i * Store::sector_size + 511] = 0xaa;
    }
    u32 const checksum = boot_checksum(boot.begin(), Store::sector_size);
    for (size_t i = 0; i < Store::sector_size / 4; ++i)
        __builtin_memcpy(boot.begin() + 11 * Store::sector_size + i * 4, &checksum, 4);
    TRY(cache.write(0, { boot.begin(), boot.size() }));
    TRY(cache.write(12 * Store::sector_size, { boot.begin(), boot.size() }));

    // ---- What goes in the heap first: the bitmap, the up-case table, the root.
    u64 const heap = heap_offset * static_cast<u64>(Store::sector_size);
    u64 const bitmap_bytes = (clusters + 7) / 8;
    u32 const bitmap_clusters = static_cast<u32>((bitmap_bytes + cluster_size - 1) / cluster_size);

    // Up-case: what the Unicode Character Database says, for those it's one for one, with runs of what doesn't change
    // said once.
    Vector<u8> table;
    auto const add_unit = [&](u16 unit) -> ErrorOr<void> {
        TRY(table.try_append(static_cast<u8>(unit)));
        return table.try_append(static_cast<u8>(unit >> 8));
    };
    u32 unchanged = 0;
    for (u32 unit = 0; unit < 0x10000; ++unit) {
        u32 upper = Unicode::simple_to_upper(unit);
        if (unit >= 0xd800 && unit < 0xe000)
            upper = unit;
        if (upper >= 0x10000)
            upper = unit;
        if (upper == unit) {
            ++unchanged;
            continue;
        }
        if (unchanged) {
            if (unchanged < 3) {
                for (u32 i = unit - unchanged; i < unit; ++i)
                    TRY(add_unit(static_cast<u16>(i)));
            } else {
                TRY(add_unit(0xffff));
                TRY(add_unit(static_cast<u16>(unchanged)));
            }
            unchanged = 0;
        }
        TRY(add_unit(static_cast<u16>(upper)));
    }
    if (unchanged) {
        TRY(add_unit(0xffff));
        TRY(add_unit(static_cast<u16>(unchanged)));
    }
    u32 const table_clusters = static_cast<u32>((table.size() + cluster_size - 1) / cluster_size);
    u32 const bitmap_first = 2;
    u32 const table_first = bitmap_first + bitmap_clusters;
    u32 const root_first = table_first + table_clusters;
    u32 const used = bitmap_clusters + table_clusters + 1;
    if (used >= clusters)
        return Error::from_errno(ENOSPC);

    TRY(cache.zero(heap, static_cast<u64>(used) * cluster_size));
    for (u32 i = 0; i < used; ++i) {
        u8 byte;
        TRY(cache.read(heap + i / 8, { &byte, 1 }));
        byte |= 1 << (i % 8);
        TRY(cache.write(heap + i / 8, { &byte, 1 }));
    }
    u64 const table_start = heap + static_cast<u64>(table_first - 2) * cluster_size;
    TRY(cache.write(table_start, { table.begin(), table.size() }));

    // ---- The FAT.
    u32 const fat_head[2] = { 0xfffffff8, 0xffffffff };
    u64 const fat = fat_offset * static_cast<u64>(Store::sector_size);
    TRY(cache.write(fat, { reinterpret_cast<u8 const*>(fat_head), sizeof(fat_head) }));
    auto const chain = [&](u32 first, u32 count) -> ErrorOr<void> {
        for (u32 i = 0; i < count; ++i) {
            u32 const value = i + 1 == count ? fat_end : first + i + 1;
            TRY(cache.write(fat + static_cast<u64>(first + i) * 4, { reinterpret_cast<u8 const*>(&value), 4 }));
        }
        return {};
    };
    TRY(chain(bitmap_first, bitmap_clusters));
    TRY(chain(table_first, table_clusters));
    TRY(chain(root_first, 1));

    // ---- The root.
    u64 const root = heap + static_cast<u64>(root_first - 2) * cluster_size;
    u64 at = root;
    auto const units = TRY(utf16_of(label));
    LabelEntry label_entry {};
    label_entry.type = units.is_empty() ? 0x03 : type_label;
    label_entry.length = static_cast<u8>(min<size_t>(units.size(), 11));
    u16 label_units[11] {};
    for (size_t i = 0; i < label_entry.length; ++i)
        label_units[i] = units[i];
    __builtin_memcpy(label_entry.text, label_units, sizeof(label_units));
    TRY(cache.write(at, { reinterpret_cast<u8 const*>(&label_entry), entry_size }));
    at += entry_size;
    BitmapEntry bitmap_entry {};
    bitmap_entry.type = type_bitmap;
    bitmap_entry.first_cluster = bitmap_first;
    bitmap_entry.length = bitmap_bytes;
    TRY(cache.write(at, { reinterpret_cast<u8 const*>(&bitmap_entry), entry_size }));
    at += entry_size;
    UpcaseEntry upcase_entry {};
    upcase_entry.type = type_upcase;
    upcase_entry.checksum = upcase_checksum(table.begin(), table.size());
    upcase_entry.first_cluster = table_first;
    upcase_entry.length = table.size();
    TRY(cache.write(at, { reinterpret_cast<u8 const*>(&upcase_entry), entry_size }));

    u32 const root_cluster = root_first;
    TRY(cache.write(96, { reinterpret_cast<u8 const*>(&root_cluster), 4 }));
    TRY(cache.write(12 * Store::sector_size + 96, { reinterpret_cast<u8 const*>(&root_cluster), 4 }));
    // (The root cluster is covered by the checksum: so it's worked out again, with that in.)
    TRY(cache.read(0, { boot.begin(), boot.size() }));
    u32 const final_checksum = boot_checksum(boot.begin(), Store::sector_size);
    for (size_t i = 0; i < Store::sector_size / 4; ++i) {
        TRY(cache.write(11 * Store::sector_size + i * 4, { reinterpret_cast<u8 const*>(&final_checksum), 4 }));
        TRY(cache.write(23 * Store::sector_size + i * 4, { reinterpret_cast<u8 const*>(&final_checksum), 4 }));
    }
    return cache.flush();
}

}
