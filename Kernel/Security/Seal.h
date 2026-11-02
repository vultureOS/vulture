/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>
#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/StringView.h>

// The sealed system: /System, /bin, /sbin, /usr (but /usr/local), /etc/ssl and the applications the
// system came with are what a signed manifest says they are, /System/Library/Seal/Manifest, which the
// kernel checks at boot with a key built into it.
//
// At boot, every entry's type, mode and owner, and every link's target, are compared; a mismatch, or
// a bad signature, stops the boot, as a system that has been changed can't be vouched for. A file's
// contents are compared with its hash the first time anyone opens or runs it: if they differ it can't
// be opened (EIO). What isn't listed, in a sealed directory, isn't there (ENOENT). While the system
// runs nothing sealed can be changed, by anyone (EROFS), and the disk it is on can't be written to
// directly.
//
// This is a hash tree of two levels, one signature over a list of files' hashes, checked one whole
// file at a time. Blocks read again later, after the page cache has let go of them, aren't checked
// again: that would take a tree down to each block.
//
// The boot word "noseal" turns all this off, and code signing with it.

namespace Kernel::Seal {

void initialize(bool enabled);
[[nodiscard]] bool is_active();

// Nothing sealed may be changed: EROFS if this is.
[[nodiscard]] ErrorOr<void> check_may_change(Inode const&);
// `child` was found in `directory`: is it meant to be there?
[[nodiscard]] ErrorOr<void> check_child(Inode const& directory, Inode const& child);
// Anyone reading it: its contents must be what the manifest says. Only reads it once.
[[nodiscard]] ErrorOr<void> check_contents(Inode&);
// Writing to this block device would go around everything above.
[[nodiscard]] bool protects_block_device(StringView name);

[[nodiscard]] bool is_sealed(Inode const&);
// Whether a program or library with this SHA-256 came with the system.
[[nodiscard]] bool is_system_code(u8 const* hash);

}
