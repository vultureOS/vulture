/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// Placement new. The replaceable allocation functions (operator new(size_t)
// and friends) are supplied by each environment's allocator.

[[nodiscard]] inline void* operator new(size_t, void* pointer) noexcept
{
    return pointer;
}

[[nodiscard]] inline void* operator new[](size_t, void* pointer) noexcept
{
    return pointer;
}
