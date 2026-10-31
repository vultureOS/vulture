/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Optional.h>
#include <LibBase/Span.h>

namespace Kernel {

// The boot archive (a ustar file the loader brought in) as the read-only
// root filesystem, until the VFS arrives (M5). It stays in memory, so file
// contents are handed out as spans of it.
class BootArchive {
public:
    struct Node {
        ReadonlyBytes data;
        bool is_directory { false };
        u32 mode { 0 }; // Permission bits.
        u64 inode { 0 };
    };

    static ErrorOr<void> initialize(ReadonlyBytes archive);
    [[nodiscard]] static bool is_available();
    [[nodiscard]] static size_t entry_count();

    // An absolute path ("/sbin/init"; "/" is the root directory).
    [[nodiscard]] static Optional<Node> lookup(StringView path);
};

}
