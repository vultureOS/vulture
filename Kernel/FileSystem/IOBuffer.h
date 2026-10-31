/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>

namespace Kernel {

// Where file data goes to or comes from: a program's memory (checked and
// fault-safe, see UserAccess.h) or a kernel buffer. Filesystems use this so
// reads land in user memory without a bounce buffer.
class IOBuffer {
public:
    static IOBuffer user(FlatPtr address) { return IOBuffer(address, true); }
    static IOBuffer kernel(void* address) { return IOBuffer(reinterpret_cast<FlatPtr>(address), false); }

    // Copies `data` into the buffer at `offset`.
    ErrorOr<void> write(size_t offset, ReadonlyBytes data) const;
    // Copies from the buffer at `offset` into `data`.
    ErrorOr<void> read(size_t offset, Bytes data) const;
    ErrorOr<void> fill_with_zeros(size_t offset, size_t count) const;

    [[nodiscard]] bool is_user() const { return m_is_user; }

private:
    IOBuffer(FlatPtr address, bool is_user)
        : m_address(address)
        , m_is_user(is_user)
    {
    }

    FlatPtr m_address;
    bool m_is_user;
};

}
