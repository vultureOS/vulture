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

namespace Base {

// Selects the allocation functions below: `new (nothrow) T` yields null
// instead of an object when memory runs out (the constructor does not run).
struct NoThrowTag {
    explicit constexpr NoThrowTag() = default;
};

inline constexpr NoThrowTag nothrow {};

}

// Non-throwing allocation, supplied by each environment that uses it.
[[nodiscard]] void* operator new(size_t, Base::NoThrowTag) noexcept;
[[nodiscard]] void* operator new[](size_t, Base::NoThrowTag) noexcept;
void operator delete(void*, Base::NoThrowTag) noexcept;
void operator delete[](void*, Base::NoThrowTag) noexcept;
