/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibVultureFS/Format.h>

namespace VultureFS {

// Where a volume's blocks live: a disk partition in the kernel, an image
// file in the host tools, memory in the tests.
class BlockStore {
public:
    virtual ~BlockStore() = default;

    [[nodiscard]] virtual u64 block_count() const = 0;
    // `buffer` holds `count` blocks.
    virtual ErrorOr<void> read_blocks(BlockIndex first, size_t count, Bytes buffer) = 0;
    virtual ErrorOr<void> write_blocks(BlockIndex first, size_t count, ReadonlyBytes buffer) = 0;
    // Returns once everything written before is on stable storage. Writes
    // between two flushes may reach the disk in any order, or not at all.
    virtual ErrorOr<void> flush() = 0;
};

}
