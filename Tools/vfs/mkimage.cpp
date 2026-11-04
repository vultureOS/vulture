/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FAT32.h"
#include "Image.h"
#include "Populate.h"
#include <LibVultureFS/Volume.h>

// mkimage OUTPUT SIZE_MIB ESP_DIRECTORY SYSTEM_IMAGE[+SYSTEM_IMAGE] [FILE...]
//
// A whole bootable disk, as a real machine has: a GPT with an EFI System
// Partition (FAT32, holding the FILEs, paths relative to ESP_DIRECTORY:
// the loader and the kernel), a partition with the system image on it, and
// its data volume filling the rest. The firmware boots it with no other disk
// attached.
//
// With more than one system, two partitions for each, for a disk that boots more
// than one kind of machine: the rest is shared equally, and the kernel finds
// its own by what's in System/Library/CoreServices/Architecture.
//
// With a kernel8.img among the FILEs, the disk is for a Raspberry Pi too, whose
// firmware also gets to find it in the old kind of partition table.

using namespace VultureFS;

namespace {

constexpr u64 esp_sectors = 300 * MiB / Tools::sector_size;

template<typename... Args>
int fail(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    Tools::StandardError error;
    error.write("mkimage: "_sv);
    format_to(error, format, args...);
    error.write("\n"_sv);
    return 1;
}

}

int main(int argc, char** argv)
{
    if (argc < 5)
        return fail("usage: mkimage OUTPUT SIZE_MIB ESP_DIRECTORY SYSTEM_IMAGE[+SYSTEM_IMAGE] [FILE...]");
    char const* output = argv[1];
    auto size_mib = StringView(argv[2]).to_number<u64>();
    if (!size_mib.has_value() || size_mib.value() < 512)
        return fail("size must be a number of MiB, at least 512");
    StringView esp_directory(argv[3]);
    Vector<Vector<u8>> archives;
    {
        ErrorOr<void> reading {};
        StringView(argv[4]).for_each_split_view('+', SplitBehavior::SkipEmpty, [&](StringView name) {
            StringBuilder path;
            path.append(name);
            path.append('\0');
            auto archive = Tools::read_file(path.view().characters_without_null_termination());
            if (archive.is_error())
                reading = archive.release_error();
            else
                archives.append(archive.release_value());
        });
        if (reading.is_error())
            return fail("cannot read {}: {}", StringView(argv[4]), reading.error());
    }
    if (archives.is_empty() || archives.size() > 2)
        return fail("one system, or two");

    Vector<Tools::FATFile> files;
    u64 esp_bytes = 0;
    for (int i = 5; i < argc; ++i) {
        StringBuilder host_path;
        host_path.append(esp_directory);
        host_path.append('/');
        host_path.append(StringView(argv[i]));
        host_path.append('\0');
        auto data = Tools::read_file(host_path.view().characters_without_null_termination());
        if (data.is_error())
            return fail("cannot read {}: {}", host_path.view(), data.error());
        esp_bytes += data.value().size();
        files.append(Tools::FATFile { MUST(String::from_view(StringView(argv[i]))), data.release_value() });
    }

    Tools::add_log_files(files);

    FILE* image = fopen(output, "w+b");
    if (!image)
        return fail("cannot create {}", StringView(output));
    u64 size = size_mib.value() * MiB;
    u64 const first_after_esp = Tools::partition_start_sector + esp_sectors;
    u64 const total_after_esp = Tools::last_usable_sector(size) - first_after_esp + 1;
    u64 const sectors_each = (total_after_esp / archives.size() / 8) * 8;
    struct Pair {
        u64 system_first;
        u64 system_count;
        u64 data_first;
        u64 data_count;
    };
    Pair pairs[2] {};
    Tools::PartitionSpec partitions[5] = {
        { Tools::efi_system_partition_type, "EFI System Partition"_sv, Tools::partition_start_sector, esp_sectors },
    };
    for (size_t i = 0; i < archives.size(); ++i) {
        auto system_sectors = Tools::system_partition_sectors(archives[i].size(), sectors_each);
        if (system_sectors.is_error()) {
            fclose(image);
            return fail("{}", system_sectors.error());
        }
        u64 const first = first_after_esp + i * sectors_each;
        pairs[i] = { first, system_sectors.value(), first + system_sectors.value(), sectors_each - system_sectors.value() };
        partitions[1 + 2 * i] = { VultureFS::system_partition_type, Tools::system_volume_name, pairs[i].system_first, pairs[i].system_count };
        partitions[2 + 2 * i] = { VultureFS::data_partition_type, Tools::data_volume_name, pairs[i].data_first, pairs[i].data_count };
    }
    ErrorOr<void> result = Tools::write_partition_table(image, size, Span<Tools::PartitionSpec const>(partitions, 1 + 2 * archives.size()));
    for (auto const& file : files) {
        if (!result.is_error() && file.path.view() == "kernel8.img"_sv)
            result = Tools::add_partition_to_mbr(image, Tools::partition_start_sector, esp_sectors);
    }
    if (!result.is_error())
        result = Tools::write_fat32(image, Tools::partition_start_sector, esp_sectors, "EFI"_sv, files);
    if (result.is_error()) {
        fclose(image);
        return fail("{}", result.error());
    }

    u64 system_bytes = 0;
    for (size_t i = 0; i < archives.size(); ++i) {
        result = Tools::write_system(image, pairs[i].system_first, pairs[i].system_count, { archives[i].begin(), archives[i].size() });
        if (!result.is_error())
            result = Tools::format_data(image, pairs[i].data_first, pairs[i].data_count, Tools::data_volume_name);
        if (result.is_error()) {
            fclose(image);
            return fail("cannot write the volumes: {}", result.error());
        }
        system_bytes += archives[i].size();
    }
    fclose(image);
    Tools::print("mkimage: {}: {} MiB; EFI System Partition: {} files ({} KiB); system: {} KiB\n",
        StringView(output), size_mib.value(), files.size(), esp_bytes / 1024, system_bytes / 1024);
    return 0;
}
