/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Image.h"
#include "Populate.h"
#include <Kernel/API/POSIX/errno.h>
#include <LibArchive/Tar.h>
#include <LibVultureFS/Check.h>
#include <LibVultureFS/Volume.h>

// mkvfs create IMAGE SIZE_MIB ARCHIVE [NAME]
//     A new GPT disk image with one vultureFS partition, holding the files
//     of the boot archive (a ustar file) plus the usual empty directories.
//
// mkvfs update IMAGE ARCHIVE
//     Brings the files of the archive into an existing image, replacing
//     those it has and leaving everything else (the user's files) alone:
//     a system update.

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

int create(char const* image_path, u64 size_mib, char const* archive_path, StringView name)
{
    auto archive = Tools::read_file(archive_path);
    if (archive.is_error())
        return fail("cannot read {}: {}", StringView(archive_path), archive.error());
    FILE* image = fopen(image_path, "w+b");
    if (!image)
        return fail("cannot create {}", StringView(image_path));
    auto partition = Tools::create_disk_image(image, size_mib * 1024 * 1024, "vultureOS"_sv);
    if (partition.is_error()) {
        fclose(image);
        return fail("{}", partition.error());
    }
    Tools::ImageStore store(image, partition.value().first_sector * Tools::sector_size, partition.value().sector_count * Tools::sector_size / block_size);
    u8 uuid[16];
    Tools::random_bytes(uuid);
    auto volume = Volume::format(store, name, uuid, Tools::host_clock);
    if (volume.is_error()) {
        fclose(image);
        return fail("cannot format: {}", volume.error());
    }
    Tools::PopulateCounts counts;
    auto added = Tools::make_skeleton(*volume.value(), counts);
    if (!added.is_error())
        added = Tools::add_archive(*volume.value(), { archive.value().begin(), archive.value().size() }, counts);
    auto committed = added.is_error() ? added : volume.value()->checkpoint();
    if (committed.is_error()) {
        fclose(image);
        return fail("cannot write the volume: {}", committed.error());
    }
    auto statistics = volume.value()->statistics();
    fclose(image);
    Tools::print("mkvfs: {}: {} MiB, {} files ({} KiB), {} directories, {} links; {} of {} blocks free\n", StringView(image_path), size_mib,
        counts.files, counts.bytes / 1024, counts.directories, counts.links, statistics.free_blocks, statistics.block_count);
    return 0;
}

int update(char const* image_path, char const* archive_path)
{
    auto archive = Tools::read_file(archive_path);
    if (archive.is_error())
        return fail("cannot read {}: {}", StringView(archive_path), archive.error());
    FILE* image = fopen(image_path, "r+b");
    if (!image)
        return fail("cannot open {}", StringView(image_path));
    auto partition = Tools::find_vulturefs_partition(image);
    if (partition.is_error()) {
        fclose(image);
        return fail("{}: {}", StringView(image_path), partition.error());
    }
    Tools::ImageStore store(image, partition.value().first_sector * Tools::sector_size, partition.value().sector_count * Tools::sector_size / block_size);
    auto volume = Volume::mount(store, Tools::host_clock);
    if (volume.is_error()) {
        fclose(image);
        return fail("cannot mount {}: {}", StringView(image_path), volume.error());
    }
    Tools::PopulateCounts counts;
    auto added = Tools::add_archive(*volume.value(), { archive.value().begin(), archive.value().size() }, counts);
    if (!added.is_error())
        added = Tools::remove_retired_files(*volume.value(), counts);
    auto committed = added.is_error() ? added : volume.value()->checkpoint();
    fclose(image);
    if (committed.is_error())
        return fail("cannot update the volume: {}", committed.error());
    Tools::print("mkvfs: {}: updated {} files ({} KiB); generation {}\n", StringView(image_path), counts.files, counts.bytes / 1024,
        volume.value()->statistics().generation);
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc >= 5 && StringView(argv[1]) == "create"_sv) {
        auto size = StringView(argv[3]).to_number<u64>();
        if (!size.has_value() || size.value() < 16)
            return fail("size must be a number of MiB, at least 16");
        return create(argv[2], size.value(), argv[4], argc >= 6 ? StringView(argv[5]) : "Vulture HD"_sv);
    }
    if (argc == 4 && StringView(argv[1]) == "update"_sv)
        return update(argv[2], argv[3]);
    return fail("usage: mkvfs create IMAGE SIZE_MIB ARCHIVE [NAME] | mkvfs update IMAGE ARCHIVE");
}
