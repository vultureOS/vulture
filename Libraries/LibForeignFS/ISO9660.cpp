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

// ISO 9660, for reading: with its long names by Joliet, or by Rock Ridge, whichever a disc has.

namespace ForeignFS {

namespace {

constexpr size_t logical_block = 2048;
constexpr u64 first_descriptor = 16 * logical_block;

constexpr u8 descriptor_primary = 1;
constexpr u8 descriptor_supplementary = 2;
constexpr u8 descriptor_end = 255;

constexpr u8 flag_hidden = 0x01;
constexpr u8 flag_directory = 0x02;

struct [[gnu::packed]] Record {
    u8 length;
    u8 extended_attribute_length;
    u32 extent_le;
    u32 extent_be;
    u32 size_le;
    u32 size_be;
    u8 date[7];
    u8 flags;
    u8 file_unit_size;
    u8 gap;
    u16 sequence_le;
    u16 sequence_be;
    u8 name_length;
};
static_assert(sizeof(Record) == 33);

i64 days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    i64 const era = (year >= 0 ? year : year - 399) / 400;
    i64 const year_of_era = year - era * 400;
    i64 const day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    i64 const day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

i64 seconds_of(u8 const* date)
{
    if (date[0] == 0 && date[1] == 0)
        return 0;
    i64 const offset = static_cast<signed char>(date[6]);
    return days_from_civil(1900 + date[0], max<int>(date[1], 1), max<int>(date[2], 1)) * 86400 + date[3] * 3600 + date[4] * 60 + date[5] - offset * 15 * 60;
}

class ISO9660Volume final : public Volume {
public:
    explicit ISO9660Volume(Store& store)
        : m_cache(store)
    {
    }

    ErrorOr<void> open();

    [[nodiscard]] StringView type_name() const override { return "cd9660"_sv; }
    [[nodiscard]] StringView label() const override { return m_label.view(); }
    [[nodiscard]] bool is_read_only() const override { return true; }
    [[nodiscard]] NodeIndex root() const override { return root_index; }
    [[nodiscard]] ErrorOr<Statistics> statistics() override { return Statistics { logical_block, m_cache.size() / logical_block, 0 }; }

    ErrorOr<NodeInfo> info(NodeIndex) override;
    ErrorOr<NodeInfo> lookup(NodeIndex directory, StringView name) override;
    ErrorOr<void> for_each_entry(NodeIndex directory, Function<Iteration(StringView, NodeInfo const&)>) override;
    ErrorOr<size_t> read(NodeIndex, u64 offset, Bytes) override;
    ErrorOr<size_t> write(NodeIndex, u64, ReadonlyBytes) override { return Error::from_errno(EROFS); }
    ErrorOr<void> truncate(NodeIndex, u64) override { return Error::from_errno(EROFS); }
    ErrorOr<NodeInfo> create(NodeIndex, StringView, bool) override { return Error::from_errno(EROFS); }
    ErrorOr<void> remove(NodeIndex, StringView) override { return Error::from_errno(EROFS); }
    ErrorOr<void> rename(NodeIndex, StringView, NodeIndex, StringView) override { return Error::from_errno(EROFS); }
    ErrorOr<void> set_times(NodeIndex, i64, i64) override { return Error::from_errno(EROFS); }
    ErrorOr<void> set_read_only(NodeIndex, bool) override { return Error::from_errno(EROFS); }
    void forget(NodeIndex) override { }
    ErrorOr<void> flush() override { return {}; }

private:
    static constexpr NodeIndex root_index = 1;

    struct Node {
        u32 extent { 0 };
        u32 size { 0 };
        bool is_directory { false };
        bool hidden { false };
        i64 modified { 0 };
        NodeIndex parent { 0 };
    };

    using Visitor = Function<ErrorOr<Iteration>(StringView name, Node const&)>;

    ErrorOr<void> walk(NodeIndex index, Visitor);
    ErrorOr<NodeIndex> index_of(Node const&);
    [[nodiscard]] NodeInfo info_of(NodeIndex index, Node const& node) const;

    SectorCache m_cache;
    String m_label;
    bool m_joliet { false };
    bool m_rock_ridge { false };
    u32 m_skip_susp { 0 };
    HashMap<NodeIndex, Node> m_nodes;
    HashMap<u32, NodeIndex> m_by_extent;
    NodeIndex m_next_index { 2 };
};

ErrorOr<void> ISO9660Volume::open()
{
    u8 descriptor[logical_block];
    Optional<Record> primary_root;
    Optional<Record> joliet_root;
    u8 primary_label[32];
    u8 joliet_label[32];
    for (u64 at = first_descriptor; at + logical_block <= m_cache.size(); at += logical_block) {
        TRY(m_cache.read(at, { descriptor, logical_block }));
        if (__builtin_memcmp(descriptor + 1, "CD001", 5) != 0)
            return Error::from_errno(EINVAL);
        if (descriptor[0] == descriptor_end)
            break;
        Record root;
        __builtin_memcpy(&root, descriptor + 156, sizeof(root));
        if (descriptor[0] == descriptor_primary) {
            primary_root = root;
            __builtin_memcpy(primary_label, descriptor + 40, 32);
        } else if (descriptor[0] == descriptor_supplementary) {
            // UCS-2, levels 1 to 3.
            u8 const* escape = descriptor + 88;
            if (escape[0] == '%' && escape[1] == '/' && (escape[2] == '@' || escape[2] == 'C' || escape[2] == 'E')) {
                joliet_root = root;
                __builtin_memcpy(joliet_label, descriptor + 40, 32);
            }
        }
    }
    if (!primary_root.has_value())
        return Error::from_errno(EINVAL);

    m_joliet = joliet_root.has_value();
    auto const& root = m_joliet ? joliet_root.value() : primary_root.value();
    Node node;
    node.extent = root.extent_le;
    node.size = root.size_le;
    node.is_directory = true;
    node.modified = seconds_of(root.date);
    TRY(m_nodes.try_set(root_index, node));
    TRY(m_by_extent.try_set(node.extent, root_index));

    if (m_joliet) {
        u16 units[16];
        size_t length = 0;
        for (size_t i = 0; i < 16; ++i) {
            u16 const unit = static_cast<u16>((joliet_label[i * 2] << 8) | joliet_label[i * 2 + 1]);
            if (unit == 0)
                break;
            units[length++] = unit;
        }
        while (length > 0 && units[length - 1] == ' ')
            --length;
        m_label = TRY(utf8_of_utf16({ units, length }));
    } else {
        size_t length = 32;
        while (length > 0 && (primary_label[length - 1] == ' ' || primary_label[length - 1] == 0))
            --length;
        m_label = TRY(String::from_view({ reinterpret_cast<char const*>(primary_label), length }));
        // Rock Ridge says it's there in the first record of the root: "SP", in what follows its name.
        u8 block[logical_block];
        TRY(m_cache.read(static_cast<u64>(node.extent) * logical_block, { block, logical_block }));
        Record dot;
        __builtin_memcpy(&dot, block, sizeof(dot));
        size_t const use_at = sizeof(Record) + dot.name_length + ((dot.name_length + 1) % 2 ? 0 : 0) + (dot.name_length % 2 == 0 ? 1 : 0);
        if (use_at + 7 <= dot.length && block[use_at] == 'S' && block[use_at + 1] == 'P' && block[use_at + 4] == 0xbe && block[use_at + 5] == 0xef) {
            m_rock_ridge = true;
            m_skip_susp = block[use_at + 6];
        }
    }
    return {};
}

ErrorOr<NodeIndex> ISO9660Volume::index_of(Node const& node)
{
    if (node.is_directory) {
        if (auto const* known = m_by_extent.get(node.extent))
            return *known;
    }
    auto const index = m_next_index++;
    TRY(m_nodes.try_set(index, node));
    if (node.is_directory)
        TRY(m_by_extent.try_set(node.extent, index));
    return index;
}

NodeInfo ISO9660Volume::info_of(NodeIndex index, Node const& node) const
{
    NodeInfo info;
    info.index = index;
    info.size = node.size;
    info.allocated = node.size;
    info.is_directory = node.is_directory;
    info.read_only = true;
    info.hidden = node.hidden;
    info.created = node.modified;
    info.modified = node.modified;
    info.accessed = node.modified;
    return info;
}

ErrorOr<NodeInfo> ISO9660Volume::info(NodeIndex index)
{
    auto const* node = m_nodes.get(index);
    if (!node)
        return Error::from_errno(ENOENT);
    return info_of(index, *node);
}

ErrorOr<void> ISO9660Volume::walk(NodeIndex index, Visitor visitor)
{
    auto const* found = m_nodes.get(index);
    if (!found)
        return Error::from_errno(ENOENT);
    Node const directory = *found;
    if (!directory.is_directory)
        return Error::from_errno(ENOTDIR);
    u8 block[logical_block];
    for (u32 block_index = 0; block_index * logical_block < directory.size; ++block_index) {
        TRY(m_cache.read((static_cast<u64>(directory.extent) + block_index) * logical_block, { block, logical_block }));
        for (size_t at = 0; at + sizeof(Record) <= logical_block;) {
            Record record;
            __builtin_memcpy(&record, block + at, sizeof(record));
            if (record.length == 0)
                break;
            if (at + record.length > logical_block)
                return Error::from_errno(EIO);
            u8 const* const raw_name = block + at + sizeof(Record);
            // These two say where they are, and aren't entries of their own.
            bool const is_self = record.name_length == 1 && (raw_name[0] == 0 || raw_name[0] == 1);
            if (!is_self) {
                Node node;
                node.extent = record.extent_le;
                node.size = record.size_le;
                node.is_directory = record.flags & flag_directory;
                node.hidden = record.flags & flag_hidden;
                node.modified = seconds_of(record.date);
                node.parent = index;
                String name;
                if (m_joliet) {
                    Vector<u16> units;
                    for (size_t i = 0; i + 1 < record.name_length; i += 2) {
                        u16 const unit = static_cast<u16>((raw_name[i] << 8) | raw_name[i + 1]);
                        if (unit == ';')
                            break;
                        TRY(units.try_append(unit));
                    }
                    name = TRY(utf8_of_utf16({ units.begin(), units.size() }));
                } else {
                    size_t length = record.name_length;
                    for (size_t i = 0; i < length; ++i) {
                        if (raw_name[i] == ';') {
                            length = i;
                            break;
                        }
                    }
                    // An extension that isn't there.
                    if (length > 0 && raw_name[length - 1] == '.')
                        --length;
                    name = TRY(String::from_view({ reinterpret_cast<char const*>(raw_name), length }));
                    if (m_rock_ridge) {
                        size_t use_at = sizeof(Record) + record.name_length + (record.name_length % 2 == 0 ? 1 : 0) + m_skip_susp;
                        StringBuilder alternate;
                        bool has_alternate = false;
                        while (use_at + 4 <= record.length) {
                            u8 const* use = block + at + use_at;
                            u8 const use_length = use[2];
                            if (use_length < 4 || use_at + use_length > record.length)
                                break;
                            if (use[0] == 'N' && use[1] == 'M' && use_length >= 5) {
                                alternate.append(StringView { reinterpret_cast<char const*>(use + 5), static_cast<size_t>(use_length - 5) });
                                has_alternate = true;
                            }
                            use_at += use_length;
                        }
                        if (has_alternate)
                            name = TRY(alternate.to_string());
                    }
                }
                if (TRY(visitor(name.view(), node)) == Iteration::Stop)
                    return {};
            }
            at += record.length;
        }
    }
    return {};
}

ErrorOr<NodeInfo> ISO9660Volume::lookup(NodeIndex directory, StringView name)
{
    if (name == "."_sv)
        return info(directory);
    if (name == ".."_sv) {
        auto const* node = m_nodes.get(directory);
        if (!node)
            return Error::from_errno(ENOENT);
        return info(node->parent ? node->parent : directory);
    }
    auto const wanted = TRY(comparable_name(name));
    Optional<NodeInfo> result;
    TRY(walk(directory, [&](StringView here, Node const& node) -> ErrorOr<Iteration> {
        auto const comparable = TRY(comparable_name(here));
        if (comparable.size() != wanted.size() || __builtin_memcmp(comparable.begin(), wanted.begin(), wanted.size() * sizeof(u16)) != 0)
            return Iteration::Continue;
        auto const index = TRY(index_of(node));
        result = info_of(index, node);
        return Iteration::Stop;
    }));
    if (!result.has_value())
        return Error::from_errno(ENOENT);
    return result.value();
}

ErrorOr<void> ISO9660Volume::for_each_entry(NodeIndex directory, Function<Iteration(StringView, NodeInfo const&)> callback)
{
    return walk(directory, [&](StringView name, Node const& node) -> ErrorOr<Iteration> {
        auto const index = TRY(index_of(node));
        return callback(name, info_of(index, node));
    });
}

ErrorOr<size_t> ISO9660Volume::read(NodeIndex index, u64 offset, Bytes buffer)
{
    auto const* node = m_nodes.get(index);
    if (!node)
        return Error::from_errno(ENOENT);
    if (node->is_directory)
        return Error::from_errno(EISDIR);
    if (offset >= node->size)
        return 0;
    size_t const count = min<u64>(buffer.size(), node->size - offset);
    TRY(m_cache.read(static_cast<u64>(node->extent) * logical_block + offset, buffer.trim(count)));
    return count;
}

}

ErrorOr<NonnullOwnPtr<Volume>> open_iso9660(Store& store)
{
    auto* volume = new (Base::nothrow) ISO9660Volume(store);
    if (!volume)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*static_cast<Volume*>(volume));
    TRY(volume->open());
    return owned;
}

}
