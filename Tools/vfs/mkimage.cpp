/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FAT32.h"
#include "Image.h"
#include "Populate.h"
#include <LibVultureFS/Volume.h>

// mkimage OUTPUT SIZE_MIB ESP_DIRECTORY ARCHIVE[+ARCHIVE] [FILE...]
//
// A whole bootable disk, as a real machine has: a GPT with an EFI System
// Partition (FAT32, holding the FILEs, paths relative to ESP_DIRECTORY:
// the loader and the kernel) and a vultureFS partition filling the rest,
// holding the files of the boot archive. The firmware boots it with no
// other disk attached.
//
// With more than one archive, a partition each, for a disk that boots more
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
        return fail("usage: mkimage OUTPUT SIZE_MIB ESP_DIRECTORY ARCHIVE[+ARCHIVE] [FILE...]");
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
        return fail("one archive, or two");

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
    u64 data_first = Tools::partition_start_sector + esp_sectors;
    u64 const data_total = Tools::last_usable_sector(size) - data_first + 1;
    u64 const data_each = (data_total / archives.size() / 8) * 8;
    Tools::PartitionSpec partitions[3] = {
        { Tools::efi_system_partition_type, "EFI System Partition"_sv, Tools::partition_start_sector, esp_sectors },
        { VultureFS::partition_type, "vultureOS"_sv, data_first, data_each },
        { VultureFS::partition_type, "vultureOS"_sv, data_first + data_each, data_each },
    };
    ErrorOr<void> result = Tools::write_partition_table(image, size, Span<Tools::PartitionSpec const>(partitions, 1 + archives.size()));
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

    Tools::PopulateCounts counts;
    for (size_t i = 0; i < archives.size(); ++i) {
        Tools::ImageStore store(image, (data_first + i * data_each) * Tools::sector_size, data_each * Tools::sector_size / block_size);
        u8 uuid[16];
        Tools::random_bytes(uuid);
        auto volume = Volume::format(store, "Vulture HD"_sv, uuid, Tools::host_clock);
        if (volume.is_error()) {
            fclose(image);
            return fail("cannot format: {}", volume.error());
        }
        result = Tools::make_skeleton(*volume.value(), counts);
        if (!result.is_error())
            result = Tools::add_archive(*volume.value(), { archives[i].begin(), archives[i].size() }, counts);
        if (!result.is_error())
            result = volume.value()->checkpoint();
        if (result.is_error()) {
            fclose(image);
            return fail("cannot write the volume: {}", result.error());
        }
    }
    fclose(image);
    Tools::print("mkimage: {}: {} MiB; EFI System Partition: {} files ({} KiB); vultureFS: {} files ({} KiB), {} directories\n",
        StringView(output), size_mib.value(), files.size(), esp_bytes / 1024, counts.files, counts.bytes / 1024, counts.directories);
    return 0;
}
