/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/FileSystem/FileSystem.h>
#include <LibForeignFS/Volume.h>

// FAT, exFAT and ISO 9660 (LibForeignFS), as the kernel's file system interface wants them.
namespace Kernel {

// What's on it, by what it starts with.
[[nodiscard]] ForeignFS::Kind recognise_file_system(BlockDevice&);
// And the name it goes by, if there's one to be had.
[[nodiscard]] ErrorOr<String> label_of_file_system(BlockDevice&);

[[nodiscard]] ErrorOr<NonnullRefPtr<FileSystem>> mount_foreign_file_system(BlockDevice&, bool read_only);

}
