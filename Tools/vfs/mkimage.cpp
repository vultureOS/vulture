/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FAT32.h"
#include "Image.h"
#include "Populate.h"
#include <LibVultureFS/Volume.h>

// mkimage OUTPUT SIZE_MIB ESP_DIRECTORY ARCHIVE [FILE...]
//
// A whole bootable disk, as a real machine has: a GPT with an EFI System
// Partition (FAT32, holding the FILEs, paths relative to ESP_DIRECTORY:
// the loader and the kernel) and a vultureFS partition filling the rest,
// holding the files of the boot archive. The firmware boots it with no
// other disk attached.

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
        return fail("usage: mkimage OUTPUT SIZE_MIB ESP_DIRECTORY ARCHIVE [FILE...]");
    char const* output = argv[1];
    auto size_mib = StringView(argv[2]).to_number<u64>();
    if (!size_mib.has_value() || size_mib.value() < 512)
        return fail("size must be a number of MiB, at least 512");
    StringView esp_directory(argv[3]);
    auto archive = Tools::read_file(argv[4]);
    if (archive.is_error())
        return fail("cannot read {}: {}", StringView(argv[4]), archive.error());

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

    FILE* image = fopen(output, "w+b");
    if (!image)
        return fail("cannot create {}", StringView(output));
    u64 size = size_mib.value() * MiB;
    u64 data_first = Tools::partition_start_sector + esp_sectors;
    u64 data_count = ((Tools::last_usable_sector(size) - data_first + 1) / 8) * 8;
    Tools::PartitionSpec const partitions[] = {
        { Tools::efi_system_partition_type, "EFI System Partition"_sv, Tools::partition_start_sector, esp_sectors },
        { VultureFS::partition_type, "vultureOS"_sv, data_first, data_count },
    };
    ErrorOr<void> result = Tools::write_partition_table(image, size, partitions);
    if (!result.is_error())
        result = Tools::write_fat32(image, Tools::partition_start_sector, esp_sectors, "EFI"_sv, files);
    if (result.is_error()) {
        fclose(image);
        return fail("{}", result.error());
    }

    Tools::ImageStore store(image, data_first * Tools::sector_size, data_count * Tools::sector_size / block_size);
    u8 uuid[16];
    Tools::random_bytes(uuid);
    auto volume = Volume::format(store, "Vulture HD"_sv, uuid, Tools::host_clock);
    if (volume.is_error()) {
        fclose(image);
        return fail("cannot format: {}", volume.error());
    }
    Tools::PopulateCounts counts;
    result = Tools::make_skeleton(*volume.value(), counts);
    if (!result.is_error())
        result = Tools::add_archive(*volume.value(), { archive.value().begin(), archive.value().size() }, counts);
    if (!result.is_error())
        result = volume.value()->checkpoint();
    fclose(image);
    if (result.is_error())
        return fail("cannot write the volume: {}", result.error());
    Tools::print("mkimage: {}: {} MiB; EFI System Partition: {} files ({} KiB); vultureFS: {} files ({} KiB), {} directories\n",
        StringView(output), size_mib.value(), files.size(), esp_bytes / 1024, counts.files, counts.bytes / 1024, counts.directories);
    return 0;
}
