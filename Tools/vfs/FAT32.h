/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Image.h"
#include <LibBase/String.h>

namespace Tools {

struct FATFile {
    String path; // Relative, '/'-separated: "EFI/BOOT/BOOTAA64.EFI".
    Vector<u8> data;
};

// Formats `sector_count` sectors of the image, from `first_sector`, as
// FAT32 with 4 KiB clusters, holding the files and the directories above
// them. Names keep their case (long names where 8.3 cannot hold them).
// The EFI System Partition of a disk image.
ErrorOr<void> write_fat32(FILE*, u64 first_sector, u64 sector_count, StringView label, Vector<FATFile> const&);

// The files the kernel keeps its logs in (see Kernel/Debug/DiskLog.h): made here, as it can't, all in one piece, and full
// of spaces for it to write over. write_fat32 gives each file clusters in one run.
void add_log_files(Vector<FATFile>&);

}
