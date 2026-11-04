/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Image.h"
#include <LibVultureFS/Volume.h>

namespace Tools {

struct PopulateCounts {
    size_t files { 0 };
    size_t directories { 0 };
    size_t links { 0 };
    u64 bytes { 0 };
    size_t removed { 0 };
};

// A system volume, from the files of a ustar archive laid out as the build lays them out. What's the user's to change
// goes in System/Library/Templates/Data, for the kernel to put on the data volume; what the system came with of
// Applications goes in System/Applications.
ErrorOr<void> make_system_volume(VultureFS::Volume&, ReadonlyBytes archive, PopulateCounts&);

// How many blocks are enough for a system volume of that archive: not very many more than that.
[[nodiscard]] u64 estimated_system_blocks(ReadonlyBytes archive);

// A system image and its data volume, on the two partitions these are (see LibVultureFS/Sealed.h).
ErrorOr<void> write_system(FILE*, u64 first_sector, u64 sector_count, ReadonlyBytes image);
ErrorOr<void> format_data(FILE*, u64 first_sector, u64 sector_count, StringView name);

// How much of a disk with this many sectors to leave for a system of this size, and the updates to it.
[[nodiscard]] ErrorOr<u64> system_partition_sectors(u64 image_size, u64 sectors_available);

// The name a system has, and the one its data has.
constexpr StringView system_volume_name = "Vulture HD"_sv;
constexpr StringView data_volume_name = "Vulture HD - Data"_sv;

}
