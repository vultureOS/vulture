/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>

namespace Kernel {

// Who a process acts as: real, effective and saved user and group IDs,
// and supplementary groups. User 0 (root) may do anything files allow
// anyone at all.
struct Credentials {
    static constexpr size_t max_groups = 16;

    u32 uid { 0 };
    u32 euid { 0 };
    u32 saved_uid { 0 };
    u32 gid { 0 };
    u32 egid { 0 };
    u32 saved_gid { 0 };
    u32 group_count { 0 };
    u32 groups[max_groups] {};

    [[nodiscard]] bool is_superuser() const { return euid == 0; }
    [[nodiscard]] bool in_group(u32 group) const;
};

// The calling process's, or null in a kernel thread (which may do
// anything).
[[nodiscard]] Credentials const* current_credentials();

// May the caller access a file with `metadata` as `mode` (R_OK, W_OK,
// X_OK, combined)? EACCES if not.
ErrorOr<void> check_access(InodeMetadata const&, int mode);

// May the caller remove `name` (with `child`'s metadata) from a directory
// (with `directory`'s)? Write and search on the directory, and with the
// sticky bit (as on /tmp) ownership of the file or the directory.
ErrorOr<void> check_removal(InodeMetadata const& directory, InodeMetadata const& child);

// May the caller change a file's permissions, times or owner? EPERM if
// not (only the owner and root may).
ErrorOr<void> check_owner(InodeMetadata const&);

}
