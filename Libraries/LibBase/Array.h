/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>

namespace Base {

// A fixed-size array with bounds-checked indexing. It is an aggregate, so
// `Array<int, 3> numbers { 1, 2, 3 };` works.
template<typename T, size_t Size>
struct Array {
    [[nodiscard]] static constexpr size_t size() { return Size; }
    [[nodiscard]] static constexpr bool is_empty() { return Size == 0; }

    [[nodiscard]] constexpr T* data() { return storage; }
    [[nodiscard]] constexpr T const* data() const { return storage; }

    [[nodiscard]] constexpr T& operator[](size_t index)
    {
        VERIFY(index < Size);
        return storage[index];
    }

    [[nodiscard]] constexpr T const& operator[](size_t index) const
    {
        VERIFY(index < Size);
        return storage[index];
    }

    [[nodiscard]] constexpr Span<T> span() { return { storage, Size }; }
    [[nodiscard]] constexpr Span<T const> span() const { return { storage, Size }; }
    constexpr operator Span<T>() { return span(); }
    constexpr operator Span<T const>() const { return span(); }

    constexpr void fill(T const& value)
    {
        for (size_t i = 0; i < Size; ++i)
            storage[i] = value;
    }

    [[nodiscard]] constexpr T* begin() { return storage; }
    [[nodiscard]] constexpr T* end() { return storage + Size; }
    [[nodiscard]] constexpr T const* begin() const { return storage; }
    [[nodiscard]] constexpr T const* end() const { return storage + Size; }

    constexpr bool operator==(Array const&) const = default;

    // Public only so Array stays an aggregate; use the accessors above.
    T storage[Size > 0 ? Size : 1];
};

template<typename T, typename... Rest>
Array(T, Rest...) -> Array<T, sizeof...(Rest) + 1>;

}

using Base::Array;
