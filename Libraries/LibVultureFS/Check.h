/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibVultureFS/BlockStore.h>

namespace VultureFS {

struct CheckReport {
    Vector<String> problems;
    u64 generation { 0 };
    u64 block_count { 0 };
    u64 used_blocks { 0 };
    u64 tree_nodes { 0 };
    u64 tree_height { 0 };
    u64 directories { 0 };
    u64 files { 0 };
    u64 symbolic_links { 0 };
    u64 orphans { 0 };
    u64 extents { 0 };
    u64 data_blocks { 0 };
    u64 snapshots { 0 };

    [[nodiscard]] bool is_clean() const { return problems.is_empty(); }
};

struct CheckOptions {
    // Reading every block of every file, to compare it with its checksum.
    bool check_data { true };
};

// Verifies, without writing anything, the checkpoint a mount would use and every
// snapshot: every checksum, the tree's shape and key order, that each block is used
// by exactly one thing or as many as a note says and that the bitmap says so exactly,
// link counts, directory parents, extents, that every name is one that could be made
// and is filed as it should be, and that every file is reachable from the root or listed
// as an orphan. Errors are for when it cannot read at all.
ErrorOr<CheckReport> check_volume(BlockStore&, CheckOptions = {});

}
