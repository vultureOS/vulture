/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Memory/UserAccess.h>

extern "C" u64 aarch64_copy_user(void* destination, void const* source, u64 size);
extern "C" i64 aarch64_copy_user_string(char* destination, char const* source, u64 capacity);

namespace {

struct ExceptionTableEntry {
    FlatPtr instruction;
    FlatPtr fixup;
};

}

extern "C" ExceptionTableEntry __ex_table_start[];
extern "C" ExceptionTableEntry __ex_table_end[];

namespace Kernel {

bool is_user_range(FlatPtr address, size_t size)
{
    FlatPtr end;
    if (__builtin_add_overflow(address, size, &end))
        return false;
    return address >= MemoryLayout::user_space_start && end <= MemoryLayout::user_space_end;
}

ErrorOr<void> copy_from_user(void* destination, FlatPtr source, size_t size)
{
    if (size == 0)
        return {};
    if (!is_user_range(source, size))
        return Error::from_errno(EFAULT);
    if (aarch64_copy_user(destination, reinterpret_cast<void const*>(source), size) != 0)
        return Error::from_errno(EFAULT);
    return {};
}

ErrorOr<void> copy_to_user(FlatPtr destination, void const* source, size_t size)
{
    if (size == 0)
        return {};
    if (!is_user_range(destination, size))
        return Error::from_errno(EFAULT);
    if (aarch64_copy_user(reinterpret_cast<void*>(destination), source, size) != 0)
        return Error::from_errno(EFAULT);
    return {};
}

ErrorOr<size_t> copy_string_from_user(FlatPtr source, char* buffer, size_t capacity)
{
    VERIFY(capacity > 0);
    // Check the first byte's address; the copy stops at the end of user
    // space through the same check, byte by byte, below.
    if (!is_user_range(source, 1))
        return Error::from_errno(EFAULT);
    size_t limit = min<size_t>(capacity, MemoryLayout::user_space_end - source);
    i64 result = aarch64_copy_user_string(buffer, reinterpret_cast<char const*>(source), limit);
    if (result == -1)
        return Error::from_errno(EFAULT);
    if (result == -2)
        return Error::from_errno(limit < capacity ? EFAULT : ENAMETOOLONG);
    return static_cast<size_t>(result);
}

Optional<FlatPtr> user_access_fixup(FlatPtr pc)
{
    for (auto* entry = __ex_table_start; entry != __ex_table_end; ++entry) {
        if (entry->instruction == pc)
            return entry->fixup;
    }
    return {};
}

}
