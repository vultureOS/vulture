/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/StringView.h>

// /dev/diskevents, which says when disks come and go, and /dev/diskimages, which makes disks of files.
namespace Kernel::DiskEvents {

ErrorOr<void> create_devices();
void disk_appeared(StringView name);
void disk_disappeared(StringView name);

}
