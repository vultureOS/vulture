/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Image.h"
#include <LibVultureFS/Check.h>
#include <LibVultureFS/Sealed.h>

// fsck.vfs [-q] IMAGE
//
// Checks every vultureFS volume in a disk image (each system and its data), or a volume by itself, without
// changing any of them. Exits with 0 when they are consistent, 1 when one is not and 2 when one can't be read.

using namespace VultureFS;

namespace {

bool s_quiet { false };

// 0, 1 or 2, as above.
int check_one(FILE* image, char const* path, StringView which, u64 byte_offset, u64 blocks)
{
    Tools::ImageStore store(image, byte_offset, blocks);
    auto report = check_volume(store);
    if (report.is_error()) {
        Tools::print("fsck.vfs: {}: {}: {}\n", StringView(path), which, report.error());
        return 2;
    }
    auto const& result = report.value();
    for (auto const& problem : result.problems)
        Tools::print("fsck.vfs: {}\n", problem.view());
    if (!s_quiet || !result.is_clean()) {
        Tools::print("fsck.vfs: {}: {}: generation {}, {} directories, {} files, {} links, {} orphans; {} extents ({} blocks); "
                     "tree of {} nodes, height {}; {} of {} blocks used: {}\n",
            StringView(path), which, result.generation, result.directories, result.files, result.symbolic_links, result.orphans, result.extents,
            result.data_blocks, result.tree_nodes, result.tree_height, result.used_blocks, result.block_count,
            result.is_clean() ? "clean"_sv : "DAMAGED"_sv);
    }
    return result.is_clean() ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    s_quiet = argc == 3 && StringView(argv[1]) == "-q"_sv;
    if (argc != 2 && !s_quiet) {
        Tools::StandardError error;
        error.write("usage: fsck.vfs [-q] IMAGE\n"_sv);
        return 2;
    }
    char const* path = argv[argc - 1];
    FILE* image = fopen(path, "rb");
    if (!image) {
        Tools::print("fsck.vfs: cannot open {}\n", StringView(path));
        return 2;
    }
    int worst = 0;
    auto const note = [&](int result) { worst = max(worst, result); };
    auto systems = Tools::find_partitions(image, system_partition_type);
    auto datas = Tools::find_partitions(image, data_partition_type);
    if (systems.is_error() || datas.is_error()) {
        // A volume by itself.
        fseek(image, 0, 2);
        note(check_one(image, path, "the volume"_sv, 0, static_cast<u64>(ftell(image)) / block_size));
        fclose(image);
        return worst;
    }
    for (auto const& system : systems.value()) {
        SealHeader header;
        u64 const offset = system.first_sector * Tools::sector_size;
        if (fseek(image, static_cast<long>(offset), 0) != 0 || fread(&header, 1, sizeof(header), image) != sizeof(header) || __builtin_memcmp(header.magic, seal_magic, sizeof(seal_magic)) != 0) {
            Tools::print("fsck.vfs: {}: the system partition doesn't have a system on it\n", StringView(path));
            note(1);
            continue;
        }
        note(check_one(image, path, "the system"_sv, offset + seal_blocks_before_volume(header) * block_size, header.volume_blocks));
    }
    for (auto const& data : datas.value())
        note(check_one(image, path, "its data"_sv, data.first_sector * Tools::sector_size, data.sector_count * Tools::sector_size / block_size));
    if (systems.value().is_empty() && datas.value().is_empty()) {
        Tools::print("fsck.vfs: {}: no vultureFS on that disk\n", StringView(path));
        worst = 2;
    }
    fclose(image);
    return worst;
}
