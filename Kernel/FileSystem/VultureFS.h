/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/FileSystem.h>

namespace Kernel {

class BlockDevice;

// Whether a partition's GPT type says it holds vultureFS.
[[nodiscard]] bool is_vulturefs_partition(BlockDevice&);

// Mounts the vultureFS volume on the device: the newest intact checkpoint,
// with orphans from before a crash deleted. File data is cached in pages;
// sync() writes them and commits a checkpoint.
ErrorOr<NonnullRefPtr<FileSystem>> mount_vulturefs(BlockDevice&);

}
