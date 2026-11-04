/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Image.h"
#include "Populate.h"
#include <Kernel/API/POSIX/errno.h>
#include <LibVultureFS/Volume.h>

// mkvfs system OUTPUT ARCHIVE
//     A system volume, not yet signed (vseal does that), of the files of the build's archive (a ustar file).
//
// mkvfs create DISK SIZE_MIB SYSTEM_IMAGE
//     A new GPT disk image: this system on one partition, and an empty data volume filling the rest. (The
//     kernel fills in what a system needs on it, when it starts.)
//
// mkvfs update DISK SYSTEM_IMAGE
//     Puts this system in place of the one on the disk, and leaves its data alone: a system update.

using namespace VultureFS;

namespace {

template<typename... Args>
int fail(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    Tools::StandardError error;
    error.write("mkvfs: "_sv);
    format_to(error, format, args...);
    error.write("\n"_sv);
    return 1;
}

extern "C" int ftruncate(int fd, long size);

ErrorOr<u64> make_system_in(FILE* file, ReadonlyBytes archive, u64 blocks, Tools::PopulateCounts& counts)
{
    if (ftruncate(fileno(file), 0) != 0 || ftruncate(fileno(file), static_cast<long>(blocks * block_size)) != 0)
        return Error::from_errno(EIO);
    Tools::ImageStore store(file, 0, blocks);
    u8 uuid[16];
    Tools::random_bytes(uuid);
    FormatOptions options;
    options.role = role_system;
    options.sealed = true;
    auto volume = TRY(Volume::format(store, Tools::system_volume_name, uuid, Tools::host_clock, options));
    counts = {};
    TRY(Tools::make_system_volume(*volume, archive, counts));
    TRY(volume->checkpoint());
    return volume->statistics().block_count - volume->statistics().free_blocks;
}

int system(char const* output_path, char const* archive_path)
{
    auto archive = Tools::read_file(archive_path);
    if (archive.is_error())
        return fail("cannot read {}: {}", StringView(archive_path), archive.error());
    ReadonlyBytes const bytes { archive.value().begin(), archive.value().size() };
    FILE* output = fopen(output_path, "w+b");
    if (!output)
        return fail("cannot create {}", StringView(output_path));
    // Once with room to spare, and again with what that took: it's kept in memory on a machine with no disk.
    Tools::PopulateCounts counts;
    auto used = make_system_in(output, bytes, Tools::estimated_system_blocks(bytes), counts);
    u64 blocks = 0;
    if (!used.is_error()) {
        blocks = used.value() + used.value() / 32 + 256;
        used = make_system_in(output, bytes, blocks, counts);
    }
    fclose(output);
    if (used.is_error())
        return fail("cannot make the system volume: {}", used.error());
    Tools::print("mkvfs: {}: {} files ({} KiB), {} directories, {} links, in {} MiB\n", StringView(output_path), counts.files, counts.bytes / 1024,
        counts.directories, counts.links, blocks * block_size / MiB);
    return 0;
}

int create(char const* disk_path, u64 size_mib, char const* image_path)
{
    auto system_image = Tools::read_file(image_path);
    if (system_image.is_error())
        return fail("cannot read {}: {}", StringView(image_path), system_image.error());
    ReadonlyBytes const image { system_image.value().begin(), system_image.value().size() };
    FILE* disk = fopen(disk_path, "w+b");
    if (!disk)
        return fail("cannot create {}", StringView(disk_path));
    u64 const size = size_mib * MiB;
    u64 const first = Tools::partition_start_sector;
    auto system_sectors = Tools::system_partition_sectors(image.size(), Tools::last_usable_sector(size) - first + 1);
    if (system_sectors.is_error()) {
        fclose(disk);
        return fail("{}", system_sectors.error());
    }
    u64 const data_first = first + system_sectors.value();
    u64 const data_sectors = (Tools::last_usable_sector(size) - data_first + 1) / 8 * 8;
    Tools::PartitionSpec const partitions[] = {
        { VultureFS::system_partition_type, Tools::system_volume_name, first, system_sectors.value() },
        { VultureFS::data_partition_type, Tools::data_volume_name, data_first, data_sectors },
    };
    auto result = Tools::write_partition_table(disk, size, partitions);
    if (!result.is_error())
        result = Tools::write_system(disk, first, system_sectors.value(), image);
    if (!result.is_error())
        result = Tools::format_data(disk, data_first, data_sectors, Tools::data_volume_name);
    fclose(disk);
    if (result.is_error())
        return fail("{}", result.error());
    Tools::print("mkvfs: {}: {} MiB, {} of them for the system, of which it takes {} MiB\n", StringView(disk_path), size_mib, system_sectors.value() * Tools::sector_size / MiB, image.size() / MiB);
    return 0;
}

int update(char const* disk_path, char const* image_path)
{
    auto system_image = Tools::read_file(image_path);
    if (system_image.is_error())
        return fail("cannot read {}: {}", StringView(image_path), system_image.error());
    FILE* disk = fopen(disk_path, "r+b");
    if (!disk)
        return fail("cannot open {}", StringView(disk_path));
    auto partitions = Tools::find_partitions(disk, VultureFS::system_partition_type);
    if (partitions.is_error() || partitions.value().is_empty()) {
        fclose(disk);
        return fail("{} has no system partition, so it needs making anew", StringView(disk_path));
    }
    auto const partition = partitions.value()[0];
    auto result = Tools::write_system(disk, partition.first_sector, partition.sector_count, { system_image.value().begin(), system_image.value().size() });
    fclose(disk);
    if (result.is_error())
        return fail("cannot update {}: {}", StringView(disk_path), result.error());
    Tools::print("mkvfs: {}: updated the system\n", StringView(disk_path));
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc == 4 && StringView(argv[1]) == "system"_sv)
        return system(argv[2], argv[3]);
    if (argc == 5 && StringView(argv[1]) == "create"_sv) {
        auto size = StringView(argv[3]).to_number<u64>();
        if (!size.has_value() || size.value() < 128)
            return fail("size must be a number of MiB, at least 128");
        return create(argv[2], size.value(), argv[4]);
    }
    if (argc == 4 && StringView(argv[1]) == "update"_sv)
        return update(argv[2], argv[3]);
    return fail("usage: mkvfs system OUTPUT ARCHIVE | mkvfs create DISK SIZE_MIB SYSTEM_IMAGE | mkvfs update DISK SYSTEM_IMAGE");
}
