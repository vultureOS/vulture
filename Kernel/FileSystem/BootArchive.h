/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>

namespace Kernel {

struct UnpackedArchive {
    size_t files { 0 };
    size_t directories { 0 };
    size_t bytes { 0 };
};

// Copies the boot archive (a ustar file the loader brought in) into the
// root filesystem, much like an initramfs: the files land in tmpfs, where
// they can be changed like any others.
ErrorOr<UnpackedArchive> unpack_boot_archive(ReadonlyBytes archive);

}
