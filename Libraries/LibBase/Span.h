/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Assertions.h>
#include <LibBase/StdLibExtras.h>

namespace Base {

// A non-owning view of contiguous elements. Indexing and slicing are bounds
// checked.
template<typename T>
class Span {
public:
    using ValueType = T;

    constexpr Span() = default;

    constexpr Span(T* data, size_t size)
        : m_data(data)
        , m_size(size)
    {
        VERIFY(data != nullptr || size == 0);
    }

    template<size_t N>
    constexpr Span(T (&array)[N])
        : m_data(array)
        , m_size(N)
    {
    }

    // Span<T> converts to Span<T const>.
    template<typename U>
    requires(IsConst<T> && SameAs<RemoveConst<T>, U>)
    constexpr Span(Span<U> const& other)
        : m_data(other.data())
        , m_size(other.size())
    {
    }

    [[nodiscard]] constexpr T* data() const { return m_data; }
    [[nodiscard]] constexpr size_t size() const { return m_size; }
    [[nodiscard]] constexpr bool is_empty() const { return m_size == 0; }
    [[nodiscard]] constexpr size_t size_in_bytes() const { return m_size * sizeof(T); }

    [[nodiscard]] constexpr T& operator[](size_t index) const
    {
        VERIFY(index < m_size);
        return m_data[index];
    }

    [[nodiscard]] constexpr T& first() const
    {
        VERIFY(m_size > 0);
        return m_data[0];
    }

    [[nodiscard]] constexpr T& last() const
    {
        VERIFY(m_size > 0);
        return m_data[m_size - 1];
    }

    [[nodiscard]] constexpr Span slice(size_t start, size_t length) const
    {
        VERIFY(start <= m_size && length <= m_size - start);
        return { m_data + start, length };
    }

    [[nodiscard]] constexpr Span slice(size_t start) const
    {
        VERIFY(start <= m_size);
        return { m_data + start, m_size - start };
    }

    // Like slice(0, length), but clamps instead of failing.
    [[nodiscard]] constexpr Span trim(size_t length) const { return { m_data, min(length, m_size) }; }

    constexpr void fill(T const& value) const
        requires(!IsConst<T>)
    {
        for (size_t i = 0; i < m_size; ++i)
            m_data[i] = value;
    }

    // Copies as many elements as fit; returns how many were copied.
    constexpr size_t copy_to(Span<RemoveConst<T>> destination) const
    {
        size_t count = min(m_size, destination.size());
        for (size_t i = 0; i < count; ++i)
            destination.data()[i] = m_data[i];
        return count;
    }

    [[nodiscard]] constexpr T* begin() const { return m_data; }
    [[nodiscard]] constexpr T* end() const { return m_data + m_size; }

private:
    T* m_data { nullptr };
    size_t m_size { 0 };
};

using Bytes = Span<u8>;
using ReadonlyBytes = Span<u8 const>;

}

using Base::Bytes;
using Base::ReadonlyBytes;
using Base::Span;
