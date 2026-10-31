/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/FileSystem.h>

namespace Kernel {

class BlockDevice;

// FAT12, FAT16 and FAT32 with long file names, read-only (the EFI System
// Partition). Fails if the device does not hold a FAT filesystem.
ErrorOr<NonnullRefPtr<FileSystem>> create_fat_file_system(BlockDevice&);

}
