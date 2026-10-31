/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Memory/Heap.h>
#include <LibBase/New.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>

namespace Kernel {

// A shared, heap-allocated block of bytes (such as a program's file, kept
// for its crash reports).
class KernelBuffer final : public RefCounted<KernelBuffer> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<KernelBuffer>> create(size_t size)
    {
        auto* data = static_cast<u8*>(kmalloc(size ? size : 1));
        if (!data)
            return Error::from_errno(ENOMEM);
        auto* buffer = new (Base::nothrow) KernelBuffer(data, size);
        if (!buffer) {
            kfree(data);
            return Error::from_errno(ENOMEM);
        }
        return adopt_ref(*buffer);
    }

    ~KernelBuffer() { kfree(m_data); }

    [[nodiscard]] Bytes bytes() { return { m_data, m_size }; }
    [[nodiscard]] ReadonlyBytes bytes() const { return { m_data, m_size }; }

private:
    KernelBuffer(u8* data, size_t size)
        : m_data(data)
        , m_size(size)
    {
    }

    u8* m_data;
    size_t m_size;
};

}
