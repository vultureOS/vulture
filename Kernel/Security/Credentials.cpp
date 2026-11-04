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

// The numbers from LibVultureFS/Format.h, which are the same for read and list, write and add_file, execute and search.
static constexpr u32 acl_right_read = 1u << 1;
static constexpr u32 acl_right_write = 1u << 2;
static constexpr u32 acl_right_execute = 1u << 3;
static constexpr u32 acl_right_delete = 1u << 4;
static constexpr u32 acl_right_delete_child = 1u << 6;
static constexpr unsigned short acl_only_inherit = 1u << 7;

static bool entry_applies(vulture_acl_entry const& entry, Credentials const& credentials)
{
    if (entry.inheritance & acl_only_inherit)
        return false;
    switch (entry.tag) {
    case 0:
        return entry.id == credentials.euid;
    case 1:
        return credentials.in_group(entry.id);
    default:
        return true;
    }
}

static ErrorOr<void> check_access_control_list(Inode& inode, Credentials const& credentials, u32& wanted)
{
    auto const entries = TRY(inode.acl());
    u32 rights = 0;
    if (wanted & read_access)
        rights |= acl_right_read;
    if (wanted & write_access)
        rights |= acl_right_write;
    if (wanted & execute_access)
        rights |= acl_right_execute;
    for (auto const& entry : entries) {
        if (!entry_applies(entry, credentials))
            continue;
        if (entry.kind == 1 && (entry.permissions & rights))
            return Error::from_errno(EACCES);
        if (entry.kind == 0)
            rights &= ~entry.permissions;
    }
    wanted = 0;
    if (rights & acl_right_read)
        wanted |= read_access;
    if (rights & acl_right_write)
        wanted |= write_access;
    if (rights & acl_right_execute)
        wanted |= execute_access;
    return {};
}

// What an access control list says about one right, of those the mode has nothing to say about: yes, no or (nothing) it doesn't say.
static ErrorOr<Optional<bool>> access_control_list_says(Inode& inode, Credentials const& credentials, u32 right)
{
    auto const entries = TRY(inode.acl());
    for (auto const& entry : entries) {
        if (entry_applies(entry, credentials) && (entry.permissions & right))
            return Optional<bool>(entry.kind == 0);
    }
    return Optional<bool> {};
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
    u32 wanted = static_cast<u32>(mode) & (read_access | write_access | execute_access);
    // An access control list goes first, and says yes or no to as much as it has something to say about: what's left
    // is for the mode. It can take away what the mode gives, even from its owner.
    if (metadata.acl_inode)
        TRY(check_access_control_list(*metadata.acl_inode, *credentials, wanted));
    u32 bits;
    if (credentials->euid == metadata.uid)
        bits = metadata.permissions >> 6;
    else if (credentials->in_group(metadata.gid))
        bits = metadata.permissions >> 3;
    else
        bits = metadata.permissions;
    if ((bits & wanted) != wanted)
        return Error::from_errno(EACCES);
    return {};
}

ErrorOr<void> check_removal(InodeMetadata const& directory, InodeMetadata const& child)
{
    if (auto const* credentials = current_credentials(); credentials && !credentials->is_superuser()) {
        if (child.acl_inode) {
            auto const says = TRY(access_control_list_says(*child.acl_inode, *credentials, acl_right_delete));
            if (says.has_value()) {
                if (!says.value())
                    return Error::from_errno(EACCES);
                return {};
            }
        }
        if (directory.acl_inode) {
            auto const says = TRY(access_control_list_says(*directory.acl_inode, *credentials, acl_right_delete_child));
            if (says.has_value()) {
                if (!says.value())
                    return Error::from_errno(EACCES);
                return {};
            }
        }
    }
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
