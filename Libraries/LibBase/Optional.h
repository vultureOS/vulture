/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Assertions.h>
#include <LibBase/New.h>
#include <LibBase/StdLibExtras.h>

namespace Base {

struct NullOptType {
    explicit constexpr NullOptType() = default;
};

inline constexpr NullOptType NullOpt {};

// A value that may be absent. Accessing an empty Optional fails VERIFY.
// Special members stay trivial when T's are, so Optional<int> is as cheap
// as a struct with a bool.
template<typename T>
class [[nodiscard]] Optional {
    static_assert(!IsLvalueReference<T>, "Optional of references is not supported");

public:
    using ValueType = T;

    constexpr Optional() { }
    constexpr Optional(NullOptType) { }

    template<typename U = T>
    requires(ConstructibleFrom<T, U &&> && !SameAs<RemoveCVReference<U>, Optional> && !SameAs<RemoveCVReference<U>, NullOptType>)
    constexpr Optional(U&& value)
        : m_value(forward<U>(value))
        , m_has_value(true)
    {
    }

    constexpr Optional(Optional const&) requires(IsTriviallyCopyable<T>)
    = default;
    constexpr Optional(Optional const& other) requires(!IsTriviallyCopyable<T> && IsConstructible<T, T const&>)
    {
        if (other.m_has_value)
            construct(other.m_value);
    }

    constexpr Optional(Optional&&) requires(IsTriviallyCopyable<T>)
    = default;
    constexpr Optional(Optional&& other) requires(!IsTriviallyCopyable<T> && IsConstructible<T, T &&>)
    {
        if (other.m_has_value)
            construct(move(other.m_value));
    }

    constexpr Optional& operator=(Optional const&) requires(IsTriviallyCopyable<T>)
    = default;
    constexpr Optional& operator=(Optional const& other) requires(!IsTriviallyCopyable<T> && IsConstructible<T, T const&>)
    {
        if (this != &other) {
            clear();
            if (other.m_has_value)
                construct(other.m_value);
        }
        return *this;
    }

    constexpr Optional& operator=(Optional&&) requires(IsTriviallyCopyable<T>)
    = default;
    constexpr Optional& operator=(Optional&& other) requires(!IsTriviallyCopyable<T> && IsConstructible<T, T &&>)
    {
        if (this != &other) {
            clear();
            if (other.m_has_value)
                construct(move(other.m_value));
        }
        return *this;
    }

    constexpr ~Optional() requires(IsTriviallyDestructible<T>)
    = default;
    constexpr ~Optional() requires(!IsTriviallyDestructible<T>) { clear(); }

    [[nodiscard]] constexpr bool has_value() const { return m_has_value; }

    [[nodiscard]] constexpr T& value() &
    {
        VERIFY(m_has_value);
        return m_value;
    }

    [[nodiscard]] constexpr T const& value() const&
    {
        VERIFY(m_has_value);
        return m_value;
    }

    [[nodiscard]] constexpr T value() && { return release_value(); }

    template<typename U>
    [[nodiscard]] constexpr T value_or(U&& fallback) const&
    {
        if (m_has_value)
            return m_value;
        return forward<U>(fallback);
    }

    [[nodiscard]] constexpr T release_value()
    {
        VERIFY(m_has_value);
        T released = move(m_value);
        clear();
        return released;
    }

    constexpr void clear()
    {
        if (m_has_value) {
            m_value.~T();
            m_has_value = false;
        }
    }

    template<typename... Args>
    constexpr T& emplace(Args&&... args)
    {
        clear();
        construct(forward<Args>(args)...);
        return m_value;
    }

    constexpr T* operator->() { return &value(); }
    constexpr T const* operator->() const { return &value(); }
    constexpr T& operator*() { return value(); }
    constexpr T const& operator*() const { return value(); }

    constexpr bool operator==(Optional const& other) const
    {
        if (m_has_value != other.m_has_value)
            return false;
        return !m_has_value || m_value == other.m_value;
    }

    template<typename U>
    requires(!SameAs<RemoveCVReference<U>, Optional> && !SameAs<RemoveCVReference<U>, NullOptType>)
    constexpr bool operator==(U const& other) const
    {
        return m_has_value && m_value == other;
    }

    constexpr bool operator==(NullOptType) const { return !m_has_value; }

private:
    template<typename... Args>
    constexpr void construct(Args&&... args)
    {
        new (__builtin_addressof(m_value)) T(forward<Args>(args)...);
        m_has_value = true;
    }

    union {
        T m_value;
    };
    bool m_has_value { false };
};

}

using Base::NullOpt;
using Base::Optional;
