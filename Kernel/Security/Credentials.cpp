/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

static constexpr int read_access = 4;
static constexpr int write_access = 2;
static constexpr int execute_access = 1;
static constexpr u32 sticky_bit = 01000;

bool Credentials::in_group(u32 group) const
{
    if (egid == group)
        return true;
    for (u32 i = 0; i < group_count; ++i) {
        if (groups[i] == group)
            return true;
    }
    return false;
}

Credentials const* current_credentials()
{
    auto* process = Process::current();
    return process ? &process->credentials() : nullptr;
}

ErrorOr<void> check_access(InodeMetadata const& metadata, int mode)
{
    auto const* credentials = current_credentials();
    if (!credentials)
        return {};
    if (credentials->is_superuser()) {
        // Root still needs some execute bit to run a file.
        if ((mode & execute_access) && metadata.type != InodeType::Directory && !(metadata.permissions & 0111))
            return Error::from_errno(EACCES);
        return {};
    }
    u32 bits;
    if (credentials->euid == metadata.uid)
        bits = metadata.permissions >> 6;
    else if (credentials->in_group(metadata.gid))
        bits = metadata.permissions >> 3;
    else
        bits = metadata.permissions;
    u32 wanted = static_cast<u32>(mode) & (read_access | write_access | execute_access);
    if ((bits & wanted) != wanted)
        return Error::from_errno(EACCES);
    return {};
}

ErrorOr<void> check_removal(InodeMetadata const& directory, InodeMetadata const& child)
{
    TRY(check_access(directory, write_access | execute_access));
    auto const* credentials = current_credentials();
    if (!credentials || credentials->is_superuser() || !(directory.permissions & sticky_bit))
        return {};
    if (credentials->euid == child.uid || credentials->euid == directory.uid)
        return {};
    return Error::from_errno(EPERM);
}

ErrorOr<void> check_owner(InodeMetadata const& metadata)
{
    auto const* credentials = current_credentials();
    if (!credentials || credentials->is_superuser() || credentials->euid == metadata.uid)
        return {};
    return Error::from_errno(EPERM);
}

}
