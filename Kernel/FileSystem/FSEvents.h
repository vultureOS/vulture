/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/fsevents.h>
#include <Kernel/FileSystem/FileSystem.h>
#include <Kernel/FileSystem/Inode.h>

// What has changed, and where: for those who keep an index of it, or a window showing it. Changes on the
// volume that keeps user data are recorded in that volume, along with the change, so what's been said
// is the same as what's on the disk, however the machine went down; anything else is only of interest
// while it's running.
namespace Kernel::FSEvents {

ErrorOr<void> create_device();
// Where changes are kept from now on.
void use_volume(FileSystem&);

// Says what's happened to this path, on whichever file system it's on. What the thing is goes in the flags.
void record(Inode const&, StringView path, u32 flags);
void record_removed(FileSystem&, InodeType, StringView path);
void record(FileSystem&, StringView path, u32 flags);

}
