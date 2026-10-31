/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/FileSystem/IOBuffer.h>
#include <Kernel/Memory/UserAccess.h>

namespace Kernel {

ErrorOr<void> IOBuffer::write(size_t offset, ReadonlyBytes data) const
{
    if (m_is_user)
        return copy_to_user(m_address + offset, data.data(), data.size());
    __builtin_memcpy(reinterpret_cast<u8*>(m_address) + offset, data.data(), data.size());
    return {};
}

ErrorOr<void> IOBuffer::read(size_t offset, Bytes data) const
{
    if (m_is_user)
        return copy_from_user(data.data(), m_address + offset, data.size());
    __builtin_memcpy(data.data(), reinterpret_cast<u8 const*>(m_address) + offset, data.size());
    return {};
}

ErrorOr<void> IOBuffer::fill_with_zeros(size_t offset, size_t count) const
{
    static u8 const zeros[512] {};
    while (count > 0) {
        size_t chunk = min(count, sizeof(zeros));
        TRY(write(offset, { zeros, chunk }));
        offset += chunk;
        count -= chunk;
    }
    return {};
}

}
