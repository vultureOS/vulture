/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/FileSystem/Inode.h>
#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/StringView.h>

// The sealed system, as a Mac has its signed system volume: everything on the volume the system starts from is what
// was signed when it was built. That's a system image (LibVultureFS/Sealed.h), on a partition or in memory.
//
// Its signature is checked with a key built into the kernel, and so is the hash of the list of hashes of its blocks,
// before anything on it is used: if either isn't right, the system doesn't start. Each block is then
// checked against its hash each time it's read from what holds it. That's after the file cache has
// let go of it too, so a change made since the machine started gets caught just the same, and all that
// happens is that what's read says EIO, while the system goes on. The volume is read-only: nothing on it
// can be changed, by anyone (EROFS), and the disk it is on can't be written to directly.
//
// The boot word "noseal" turns this all off, and code signing with it. It's still read-only.

namespace Kernel::Seal {

// What's on `device`, as a device of its own that only gives what was signed. An error if it's no system image
// (ENOTSUP); or, when `checking`, if it isn't signed by this system's key (EPERM), or isn't what was signed (EIO).
[[nodiscard]] ErrorOr<NonnullRefPtr<BlockDevice>> open_system_image(BlockDevice&, bool checking);

// That volume is now the root. With it `enabled`, the seal is on.
void initialize(FileSystem& volume, bool enabled);
[[nodiscard]] bool is_active();

// Nothing sealed may be changed: EROFS if this is.
[[nodiscard]] ErrorOr<void> check_may_change(Inode const&);
// Writing to this block device would go around everything above.
[[nodiscard]] bool protects_block_device(StringView name);

[[nodiscard]] bool is_sealed(Inode const&);
// Whether a program or library with this SHA-256 came with the system.
[[nodiscard]] bool is_system_code(u8 const* hash);

// The system can't be vouched for: this stops it.
[[noreturn]] void broken(StringView what);

}
