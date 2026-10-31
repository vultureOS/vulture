/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FAT32.h"
#include <Kernel/API/POSIX/errno.h>
#include <LibBase/CharacterTypes.h>
#include <LibBase/OwnPtr.h>

namespace Tools {

namespace {

constexpr u32 sectors_per_cluster = 8;
constexpr u32 cluster_size = sectors_per_cluster * sector_size;
constexpr u32 reserved_sectors = 32;
constexpr u32 fat_count = 2;
constexpr u32 end_of_chain = 0x0fff'ffff;
constexpr u8 attribute_directory = 0x10;
constexpr u8 attribute_volume = 0x08;
constexpr u8 attribute_archive = 0x20;
constexpr u8 attribute_long_name = 0x0f;

struct [[gnu::packed]] DirectoryRecord {
    u8 name[11];
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
static_assert(sizeof(DirectoryRecord) == 32);

struct [[gnu::packed]] LongNameRecord {
    u8 order;
    u16 name1[5];
    u8 attributes;
    u8 type;
    u8 checksum;
    u16 name2[6];
    u16 cluster;
    u16 name3[2];
};
static_assert(sizeof(LongNameRecord) == 32);

struct Node {
    String name;
    bool is_directory { false };
    Vector<u8> const* data { nullptr };
    Vector<OwnPtr<Node>> children;
    u8 short_name[11] {};
    size_t long_name_records { 0 };
    u32 first_cluster { 0 };
    u32 cluster_count { 0 };

    Node* find(StringView child_name)
    {
        for (auto& child : children) {
            if (child->name.view() == child_name)
                return child.ptr();
        }
        return nullptr;
    }
};

bool is_short_name_character(char c)
{
    return Base::is_ascii_upper_alpha(static_cast<u8>(c)) || Base::is_ascii_digit(static_cast<u8>(c)) || c == '_' || c == '-' || c == '~' || c == '!' || c == '#' || c == '$' || c == '%' || c == '&';
}

// Whether `name` is a valid 8.3 name exactly as written (upper case).
bool fits_short_name(StringView name, u8 (&short_name)[11])
{
    for (auto& byte : short_name)
        byte = ' ';
    auto dot = name.find('.');
    StringView base = dot.has_value() ? name.substring_view(0, dot.value()) : name;
    StringView extension = dot.has_value() ? name.substring_view(dot.value() + 1) : StringView {};
    if (base.is_empty() || base.length() > 8 || extension.length() > 3 || extension.contains('.'))
        return false;
    for (size_t i = 0; i < base.length(); ++i) {
        if (!is_short_name_character(base[i]))
            return false;
        short_name[i] = static_cast<u8>(base[i]);
    }
    for (size_t i = 0; i < extension.length(); ++i) {
        if (!is_short_name_character(extension[i]))
            return false;
        short_name[8 + i] = static_cast<u8>(extension[i]);
    }
    return true;
}

// "vulture.bootarchive" -> "VULTUR~1BOO" (unique among `siblings`).
void make_short_name(StringView name, Vector<OwnPtr<Node>> const& siblings, Node const* self, u8 (&short_name)[11])
{
    auto dot = name.find_last('.');
    StringView base = dot.has_value() && dot.value() > 0 ? name.substring_view(0, dot.value()) : name;
    StringView extension = dot.has_value() && dot.value() > 0 ? name.substring_view(dot.value() + 1) : StringView {};
    u8 basis[8];
    size_t basis_length = 0;
    for (size_t i = 0; i < base.length() && basis_length < 6; ++i) {
        char c = static_cast<char>(Base::to_ascii_uppercase(static_cast<u8>(base[i])));
        if (is_short_name_character(c))
            basis[basis_length++] = static_cast<u8>(c);
    }
    if (basis_length == 0)
        basis[basis_length++] = '_';
    for (u32 number = 1; number < 1000000; ++number) {
        for (auto& byte : short_name)
            byte = ' ';
        char digits[8];
        size_t digit_count = 0;
        for (u32 n = number; n > 0; n /= 10)
            digits[digit_count++] = static_cast<char>('0' + n % 10);
        size_t keep = min<size_t>(basis_length, 7 - digit_count);
        for (size_t i = 0; i < keep; ++i)
            short_name[i] = basis[i];
        short_name[keep] = '~';
        for (size_t i = 0; i < digit_count; ++i)
            short_name[keep + 1 + i] = static_cast<u8>(digits[digit_count - 1 - i]);
        size_t extension_length = 0;
        for (size_t i = 0; i < extension.length() && extension_length < 3; ++i) {
            char c = static_cast<char>(Base::to_ascii_uppercase(static_cast<u8>(extension[i])));
            if (is_short_name_character(c))
                short_name[8 + extension_length++] = static_cast<u8>(c);
        }
        bool taken = false;
        for (auto const& sibling : siblings)
            taken = taken || (sibling.ptr() != self && __builtin_memcmp(sibling->short_name, short_name, 11) == 0);
        if (!taken)
            return;
    }
}

u8 short_name_checksum(u8 const (&short_name)[11])
{
    u8 sum = 0;
    for (u8 byte : short_name)
        sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + byte);
    return sum;
}

size_t entries_in(Node const& directory, bool is_root)
{
    size_t count = is_root ? 1 : 2; // Volume label, or "." and "..".
    for (auto const& child : directory.children)
        count += 1 + child->long_name_records;
    return count;
}

ErrorOr<void> write_at(FILE* file, u64 offset, void const* data, size_t size)
{
    if (fseek(file, static_cast<long>(offset), 0) != 0 || fwrite(data, 1, size, file) != size)
        return Error::from_errno(EIO);
    return {};
}

// FAT dates and times, from nanoseconds since 1970.
void fat_time(i64 nanoseconds, u16& date, u16& time)
{
    i64 seconds = nanoseconds / 1'000'000'000;
    i64 days = seconds / 86400;
    i64 rest = seconds % 86400;
    // Civil date from days since 1970 (Howard Hinnant's algorithm).
    i64 z = days + 719468;
    i64 era = z / 146097;
    i64 day_of_era = z - era * 146097;
    i64 year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    i64 year = year_of_era + era * 400;
    i64 day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    i64 month_index = (5 * day_of_year + 2) / 153;
    i64 day = day_of_year - (153 * month_index + 2) / 5 + 1;
    i64 month = month_index < 10 ? month_index + 3 : month_index - 9;
    year += month <= 2;
    if (year < 1980)
        year = 1980;
    date = static_cast<u16>(((year - 1980) << 9) | (month << 5) | day);
    time = static_cast<u16>(((rest / 3600) << 11) | (((rest / 60) % 60) << 5) | ((rest % 60) / 2));
}

class Writer {
public:
    Writer(FILE* file, u64 first_sector, u64 sector_count)
        : m_file(file)
        , m_offset(first_sector * sector_size)
        , m_first_sector(first_sector)
        , m_sector_count(sector_count)
    {
    }

    ErrorOr<void> write(StringView label, Vector<FATFile> const& files);

private:
    ErrorOr<void> add_file(FATFile const&);
    void name_children(Node&);
    void allocate(Node&, bool is_root);
    ErrorOr<void> write_contents(Node&, Node const* parent, bool is_root);
    [[nodiscard]] u64 cluster_offset(u32 cluster) const { return m_offset + (m_data_start + static_cast<u64>(cluster - 2) * sectors_per_cluster) * sector_size; }

    FILE* m_file;
    u64 m_offset;
    u64 m_first_sector;
    u64 m_sector_count;
    u32 m_fat_size { 0 };
    u32 m_cluster_total { 0 };
    u64 m_data_start { 0 };
    u32 m_next_cluster { 2 };
    Vector<u32> m_fat;
    Node m_root;
    u8 m_label[11] {};
    u16 m_date { 0 };
    u16 m_time { 0 };
};

ErrorOr<void> Writer::add_file(FATFile const& file)
{
    Node* directory = &m_root;
    StringView path = file.path.view();
    for (auto slash = path.find('/'); slash.has_value(); slash = path.find('/')) {
        StringView component = path.substring_view(0, slash.value());
        path = path.substring_view(slash.value() + 1);
        if (component.is_empty())
            continue;
        auto* next = directory->find(component);
        if (!next) {
            auto node = TRY(try_make<Node>());
            node->name = TRY(String::from_view(component));
            node->is_directory = true;
            next = node.ptr();
            TRY(directory->children.try_append(OwnPtr<Node>(move(node))));
        }
        if (!next->is_directory)
            return Error::from_errno(ENOTDIR);
        directory = next;
    }
    if (path.is_empty() || directory->find(path))
        return Error::from_errno(EEXIST);
    auto node = TRY(try_make<Node>());
    node->name = TRY(String::from_view(path));
    node->data = &file.data;
    TRY(directory->children.try_append(OwnPtr<Node>(move(node))));
    return {};
}

void Writer::name_children(Node& directory)
{
    for (auto& child : directory.children) {
        if (!fits_short_name(child->name.view(), child->short_name)) {
            make_short_name(child->name.view(), directory.children, child.ptr(), child->short_name);
            child->long_name_records = (child->name.length() + 12) / 13;
        }
        if (child->is_directory)
            name_children(*child);
    }
}

void Writer::allocate(Node& node, bool is_root)
{
    u64 bytes = node.is_directory ? entries_in(node, is_root) * sizeof(DirectoryRecord) : node.data->size();
    node.cluster_count = static_cast<u32>((bytes + cluster_size - 1) / cluster_size);
    if (node.is_directory && node.cluster_count == 0)
        node.cluster_count = 1;
    if (node.cluster_count > 0) {
        node.first_cluster = m_next_cluster;
        for (u32 i = 0; i < node.cluster_count; ++i)
            m_fat[node.first_cluster + i] = i + 1 < node.cluster_count ? node.first_cluster + i + 1 : end_of_chain;
        m_next_cluster += node.cluster_count;
    }
    for (auto& child : node.children)
        allocate(*child, false);
}

ErrorOr<void> Writer::write_contents(Node& node, Node const* parent, bool is_root)
{
    if (!node.is_directory) {
        if (node.cluster_count > 0)
            TRY(write_at(m_file, cluster_offset(node.first_cluster), node.data->begin(), node.data->size()));
        return {};
    }
    Vector<u8> table;
    TRY(table.try_resize(static_cast<size_t>(node.cluster_count) * cluster_size));
    __builtin_memset(table.begin(), 0, table.size());
    size_t index = 0;
    auto record_at = [&](size_t i) { return reinterpret_cast<DirectoryRecord*>(table.begin() + i * sizeof(DirectoryRecord)); };
    auto fill = [&](DirectoryRecord& record, u8 const* name, u8 attributes, u32 cluster, u32 size) {
        __builtin_memcpy(record.name, name, 11);
        record.attributes = attributes;
        record.creation_time = record.modification_time = m_time;
        record.creation_date = record.modification_date = record.access_date = m_date;
        record.cluster_high = static_cast<u16>(cluster >> 16);
        record.cluster_low = static_cast<u16>(cluster);
        record.size = size;
    };
    if (is_root) {
        fill(*record_at(index++), m_label, attribute_volume, 0, 0);
    } else {
        u8 dot[11], dot_dot[11];
        __builtin_memset(dot, ' ', 11);
        __builtin_memset(dot_dot, ' ', 11);
        dot[0] = dot_dot[0] = dot_dot[1] = '.';
        fill(*record_at(index++), dot, attribute_directory, node.first_cluster, 0);
        // ".." of a directory in the root points at cluster 0.
        fill(*record_at(index++), dot_dot, attribute_directory, parent && parent != &m_root ? parent->first_cluster : 0, 0);
    }
    for (auto& child : node.children) {
        u8 checksum = short_name_checksum(child->short_name);
        for (size_t part = child->long_name_records; part > 0; --part) {
            auto* record = reinterpret_cast<LongNameRecord*>(record_at(index++));
            record->order = static_cast<u8>(part | (part == child->long_name_records ? 0x40 : 0));
            record->attributes = attribute_long_name;
            record->checksum = checksum;
            u16 characters[13];
            for (size_t i = 0; i < 13; ++i) {
                size_t position = (part - 1) * 13 + i;
                if (position < child->name.length())
                    characters[i] = static_cast<u8>(child->name.view()[position]);
                else if (position == child->name.length())
                    characters[i] = 0;
                else
                    characters[i] = 0xffff;
            }
            __builtin_memcpy(record->name1, characters, sizeof(record->name1));
            __builtin_memcpy(record->name2, characters + 5, sizeof(record->name2));
            __builtin_memcpy(record->name3, characters + 11, sizeof(record->name3));
        }
        fill(*record_at(index++), child->short_name, child->is_directory ? attribute_directory : attribute_archive, child->first_cluster,
            child->is_directory ? 0 : static_cast<u32>(child->data->size()));
    }
    TRY(write_at(m_file, cluster_offset(node.first_cluster), table.begin(), table.size()));
    for (auto& child : node.children)
        TRY(write_contents(*child, &node, false));
    return {};
}

ErrorOr<void> Writer::write(StringView label, Vector<FATFile> const& files)
{
    // FAT size and cluster count depend on each other: settle them.
    u64 available = m_sector_count - reserved_sectors;
    m_fat_size = 1;
    for (int round = 0; round < 8; ++round) {
        u64 clusters = (available - fat_count * static_cast<u64>(m_fat_size)) / sectors_per_cluster;
        m_fat_size = static_cast<u32>(((clusters + 2) * 4 + sector_size - 1) / sector_size);
    }
    m_cluster_total = static_cast<u32>((available - fat_count * static_cast<u64>(m_fat_size)) / sectors_per_cluster);
    if (m_cluster_total < 65525)
        return Error::from_string_literal("partition too small for FAT32");
    m_data_start = reserved_sectors + fat_count * static_cast<u64>(m_fat_size);
    TRY(m_fat.try_resize(m_cluster_total + 2));
    for (auto& entry : m_fat)
        entry = 0;
    m_fat[0] = 0x0fff'fff8;
    m_fat[1] = end_of_chain;
    __builtin_memset(m_label, ' ', sizeof(m_label));
    for (size_t i = 0; i < label.length() && i < 11; ++i)
        m_label[i] = static_cast<u8>(Base::to_ascii_uppercase(static_cast<u8>(label[i])));
    fat_time(host_clock(), m_date, m_time);

    m_root.is_directory = true;
    for (auto const& file : files)
        TRY(add_file(file));
    name_children(m_root);
    allocate(m_root, true);
    if (m_next_cluster > m_cluster_total + 2)
        return Error::from_string_literal("the files do not fit the partition");

    // Boot sector, FSInfo, and their backups at sectors 6 and 7.
    u8 boot[sector_size] {};
    u8 const jump[3] = { 0xeb, 0x58, 0x90 };
    __builtin_memcpy(boot, jump, 3);
    __builtin_memcpy(boot + 3, "VULTURE ", 8);
    auto put16 = [](u8* at, u16 value) { at[0] = static_cast<u8>(value); at[1] = static_cast<u8>(value >> 8); };
    auto put32 = [](u8* at, u32 value) { for (int i = 0; i < 4; ++i) at[i] = static_cast<u8>(value >> (8 * i)); };
    put16(boot + 11, sector_size);
    boot[13] = sectors_per_cluster;
    put16(boot + 14, reserved_sectors);
    boot[16] = fat_count;
    boot[21] = 0xf8;
    put16(boot + 24, 32);
    put16(boot + 26, 64);
    put32(boot + 28, static_cast<u32>(m_first_sector));
    put32(boot + 32, static_cast<u32>(m_sector_count));
    put32(boot + 36, m_fat_size);
    put32(boot + 44, m_root.first_cluster);
    put16(boot + 48, 1); // FSInfo sector.
    put16(boot + 50, 6); // Backup boot sector.
    boot[64] = 0x80;
    boot[66] = 0x29;
    u8 serial[4];
    random_bytes(serial);
    __builtin_memcpy(boot + 67, serial, 4);
    __builtin_memcpy(boot + 71, m_label, 11);
    __builtin_memcpy(boot + 82, "FAT32   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xaa;
    u8 information[sector_size] {};
    put32(information, 0x4161'5252);
    put32(information + 484, 0x6141'7272);
    put32(information + 488, m_cluster_total + 2 - m_next_cluster);
    put32(information + 492, m_next_cluster);
    put32(information + 508, 0xaa55'0000);
    TRY(write_at(m_file, m_offset, boot, sector_size));
    TRY(write_at(m_file, m_offset + sector_size, information, sector_size));
    TRY(write_at(m_file, m_offset + 6 * sector_size, boot, sector_size));
    TRY(write_at(m_file, m_offset + 7 * sector_size, information, sector_size));

    // Both FATs: only the used part is written; the rest of the file reads
    // as zeros (free clusters).
    size_t used_bytes = static_cast<size_t>(m_next_cluster) * 4;
    for (u32 copy = 0; copy < fat_count; ++copy)
        TRY(write_at(m_file, m_offset + (reserved_sectors + copy * static_cast<u64>(m_fat_size)) * sector_size, m_fat.begin(), used_bytes));
    return write_contents(m_root, nullptr, true);
}

}

ErrorOr<void> write_fat32(FILE* file, u64 first_sector, u64 sector_count, StringView label, Vector<FATFile> const& files)
{
    auto writer = TRY(try_make<Writer>(file, first_sector, sector_count));
    return writer->write(label, files);
}

}
