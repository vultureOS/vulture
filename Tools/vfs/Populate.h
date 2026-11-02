/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibVultureFS/Volume.h>

namespace Tools {

struct PopulateCounts {
    size_t files { 0 };
    size_t directories { 0 };
    size_t links { 0 };
    u64 bytes { 0 };
    size_t removed { 0 };
};

// The top of the macOS-style hierarchy and the mount points (/dev, /tmp,
// /Volumes...), made even when the archive has nothing in them.
ErrorOr<void> make_skeleton(VultureFS::Volume&, PopulateCounts&);

// Adds the files of a ustar archive, replacing what is there under the
// same names (a system update) and leaving everything else alone.
ErrorOr<void> add_archive(VultureFS::Volume&, ReadonlyBytes archive, PopulateCounts&);
// Takes away what earlier systems installed under names no longer used, which would
// otherwise stay on a kept disk: the rest of a disk is its owner's, and left alone.
ErrorOr<void> remove_retired_files(VultureFS::Volume&, PopulateCounts&);

}
