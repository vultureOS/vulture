/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/TypeTraits.h>

namespace Base {

template<typename T>
constexpr RemoveReference<T>&& move(T&& value) noexcept
{
    return static_cast<RemoveReference<T>&&>(value);
}

template<typename T>
constexpr T&& forward(RemoveReference<T>& value) noexcept
{
    return static_cast<T&&>(value);
}

template<typename T>
constexpr T&& forward(RemoveReference<T>&& value) noexcept
{
    static_assert(!IsLvalueReference<T>, "Cannot forward an rvalue as an lvalue");
    return static_cast<T&&>(value);
}

template<typename T>
T&& declval() noexcept;

template<typename T, typename U = T>
constexpr T exchange(T& slot, U&& new_value)
{
    T old_value = move(slot);
    slot = forward<U>(new_value);
    return old_value;
}

template<typename T>
constexpr void swap(T& a, T& b)
{
    T temporary = move(a);
    a = move(b);
    b = move(temporary);
}

template<typename T>
constexpr T min(T a, T b)
{
    return b < a ? b : a;
}

template<typename T>
constexpr T max(T a, T b)
{
    return a < b ? b : a;
}

template<typename T>
constexpr T clamp(T value, T low, T high)
{
    return value < low ? low : (high < value ? high : value);
}

template<Enum E>
constexpr UnderlyingType<E> to_underlying(E value)
{
    return static_cast<UnderlyingType<E>>(value);
}

template<typename To, typename From>
requires(sizeof(To) == sizeof(From) && IsTriviallyCopyable<To> && IsTriviallyCopyable<From>)
constexpr To bit_cast(From const& from)
{
    return __builtin_bit_cast(To, from);
}

template<typename T, size_t N>
constexpr size_t array_size(T const (&)[N])
{
    return N;
}

template<UnsignedIntegral T>
constexpr bool is_power_of_two(T value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

// `alignment` must be a power of two.
template<UnsignedIntegral T>
constexpr T align_up(T value, T alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

template<UnsignedIntegral T>
constexpr T align_down(T value, T alignment)
{
    return value & ~(alignment - 1);
}

}

using Base::align_down;
using Base::align_up;
using Base::array_size;
using Base::bit_cast;
using Base::clamp;
using Base::exchange;
using Base::forward;
using Base::max;
using Base::min;
using Base::move;
using Base::swap;
using Base::to_underlying;
