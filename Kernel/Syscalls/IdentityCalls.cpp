/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>

// User and group IDs (POSIX semantics): root may set any; others may only
// switch among their real, effective and saved IDs.

namespace Kernel {

static u32 as_id(u64 value) { return static_cast<u32>(value); }

ErrorOr<FlatPtr> Process::sys_getuid(TrapFrame&, SyscallArguments const&) { return m_credentials.uid; }
ErrorOr<FlatPtr> Process::sys_geteuid(TrapFrame&, SyscallArguments const&) { return m_credentials.euid; }
ErrorOr<FlatPtr> Process::sys_getgid(TrapFrame&, SyscallArguments const&) { return m_credentials.gid; }
ErrorOr<FlatPtr> Process::sys_getegid(TrapFrame&, SyscallArguments const&) { return m_credentials.egid; }

ErrorOr<FlatPtr> Process::sys_setuid(TrapFrame&, SyscallArguments const& arguments)
{
    u32 uid = as_id(arguments.values[0]);
    if (m_credentials.is_superuser()) {
        m_credentials.uid = m_credentials.euid = m_credentials.saved_uid = uid;
        return 0;
    }
    if (uid != m_credentials.uid && uid != m_credentials.saved_uid)
        return Error::from_errno(EPERM);
    m_credentials.euid = uid;
    return 0;
}

ErrorOr<FlatPtr> Process::sys_setgid(TrapFrame&, SyscallArguments const& arguments)
{
    u32 gid = as_id(arguments.values[0]);
    if (m_credentials.is_superuser()) {
        m_credentials.gid = m_credentials.egid = m_credentials.saved_gid = gid;
        return 0;
    }
    if (gid != m_credentials.gid && gid != m_credentials.saved_gid)
        return Error::from_errno(EPERM);
    m_credentials.egid = gid;
    return 0;
}

ErrorOr<FlatPtr> Process::sys_seteuid(TrapFrame&, SyscallArguments const& arguments)
{
    u32 uid = as_id(arguments.values[0]);
    if (!m_credentials.is_superuser() && uid != m_credentials.uid && uid != m_credentials.saved_uid)
        return Error::from_errno(EPERM);
    m_credentials.euid = uid;
    return 0;
}

ErrorOr<FlatPtr> Process::sys_setegid(TrapFrame&, SyscallArguments const& arguments)
{
    u32 gid = as_id(arguments.values[0]);
    if (!m_credentials.is_superuser() && gid != m_credentials.gid && gid != m_credentials.saved_gid)
        return Error::from_errno(EPERM);
    m_credentials.egid = gid;
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getgroups(TrapFrame&, SyscallArguments const& arguments)
{
    size_t size = arguments.values[0];
    FlatPtr list = arguments.values[1];
    if (size == 0)
        return m_credentials.group_count;
    if (size < m_credentials.group_count)
        return Error::from_errno(EINVAL);
    TRY(copy_to_user(list, m_credentials.groups, m_credentials.group_count * sizeof(u32)));
    return m_credentials.group_count;
}

ErrorOr<FlatPtr> Process::sys_setgroups(TrapFrame&, SyscallArguments const& arguments)
{
    size_t size = arguments.values[0];
    FlatPtr list = arguments.values[1];
    if (!m_credentials.is_superuser())
        return Error::from_errno(EPERM);
    if (size > Credentials::max_groups)
        return Error::from_errno(EINVAL);
    u32 groups[Credentials::max_groups];
    TRY(copy_from_user(groups, list, size * sizeof(u32)));
    for (size_t i = 0; i < size; ++i)
        m_credentials.groups[i] = groups[i];
    m_credentials.group_count = static_cast<u32>(size);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_umask(TrapFrame&, SyscallArguments const& arguments)
{
    u32 old = m_umask;
    m_umask = static_cast<u32>(arguments.values[0]) & 0777;
    return old;
}

}
