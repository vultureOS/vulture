/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Image.h"
#include <LibVultureFS/Check.h>

// fsck.vfs [-q] IMAGE
//
// Checks the vultureFS volume in a disk image (its vultureFS partition) or
// a bare volume image, without changing it. Exits with 0 when the volume
// is consistent, 1 when it is not and 2 when it cannot be read.

using namespace VultureFS;

int main(int argc, char** argv)
{
    bool quiet = argc == 3 && StringView(argv[1]) == "-q"_sv;
    if (argc != 2 && !quiet) {
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
    auto partition = Tools::find_vulturefs_partition(image);
    if (partition.is_error()) {
        Tools::print("fsck.vfs: {}: {}\n", StringView(path), partition.error());
        fclose(image);
        return 2;
    }
    Tools::ImageStore store(image, partition.value().first_sector * Tools::sector_size, partition.value().sector_count * Tools::sector_size / block_size);
    auto report = check_volume(store);
    fclose(image);
    if (report.is_error()) {
        Tools::print("fsck.vfs: {}: {}\n", StringView(path), report.error());
        return 2;
    }
    auto const& result = report.value();
    for (auto const& problem : result.problems)
        Tools::print("fsck.vfs: {}\n", problem.view());
    if (!quiet || !result.is_clean()) {
        Tools::print("fsck.vfs: {}: generation {}, {} directories, {} files, {} links, {} orphans; {} extents ({} blocks); "
                     "tree of {} nodes, height {}; {} of {} blocks used: {}\n",
            StringView(path), result.generation, result.directories, result.files, result.symbolic_links, result.orphans, result.extents,
            result.data_blocks, result.tree_nodes, result.tree_height, result.used_blocks, result.block_count,
            result.is_clean() ? "clean"_sv : "DAMAGED"_sv);
    }
    return result.is_clean() ? 0 : 1;
}
