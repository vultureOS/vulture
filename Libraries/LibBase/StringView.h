/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/CharacterTypes.h>
#include <LibBase/NumericLimits.h>
#include <LibBase/Optional.h>
#include <LibBase/Span.h>

namespace Base {

enum class SplitBehavior {
    SkipEmpty,
    KeepEmpty,
};

// A non-owning view of UTF-8 text. It does not need to be null-terminated.
class StringView {
public:
    constexpr StringView() = default;

    constexpr StringView(char const* characters, size_t length)
        : m_characters(characters)
        , m_length(length)
    {
        VERIFY(characters != nullptr || length == 0);
    }

    // Implicit so string literals and C strings can be passed directly.
    constexpr StringView(char const* null_terminated)
        : m_characters(null_terminated)
        , m_length(null_terminated ? length_of(null_terminated) : 0)
    {
    }

    explicit StringView(ReadonlyBytes bytes)
        : m_characters(reinterpret_cast<char const*>(bytes.data()))
        , m_length(bytes.size())
    {
    }

    [[nodiscard]] constexpr char const* characters_without_null_termination() const { return m_characters; }
    [[nodiscard]] constexpr size_t length() const { return m_length; }
    [[nodiscard]] constexpr bool is_empty() const { return m_length == 0; }

    [[nodiscard]] ReadonlyBytes bytes() const { return { reinterpret_cast<u8 const*>(m_characters), m_length }; }

    [[nodiscard]] constexpr char operator[](size_t index) const
    {
        VERIFY(index < m_length);
        return m_characters[index];
    }

    [[nodiscard]] constexpr char const* begin() const { return m_characters; }
    [[nodiscard]] constexpr char const* end() const { return m_characters + m_length; }

    [[nodiscard]] constexpr StringView substring_view(size_t start, size_t length) const
    {
        VERIFY(start <= m_length && length <= m_length - start);
        return { m_characters + start, length };
    }

    [[nodiscard]] constexpr StringView substring_view(size_t start) const
    {
        VERIFY(start <= m_length);
        return { m_characters + start, m_length - start };
    }

    [[nodiscard]] constexpr bool operator==(StringView other) const
    {
        if (m_length != other.m_length)
            return false;
        for (size_t i = 0; i < m_length; ++i) {
            if (m_characters[i] != other.m_characters[i])
                return false;
        }
        return true;
    }

    // Byte-wise comparison: negative, zero or positive.
    [[nodiscard]] constexpr int compare(StringView other) const
    {
        size_t common = min(m_length, other.m_length);
        for (size_t i = 0; i < common; ++i) {
            auto a = static_cast<u8>(m_characters[i]);
            auto b = static_cast<u8>(other.m_characters[i]);
            if (a != b)
                return a < b ? -1 : 1;
        }
        if (m_length == other.m_length)
            return 0;
        return m_length < other.m_length ? -1 : 1;
    }

    [[nodiscard]] constexpr bool operator<(StringView other) const { return compare(other) < 0; }

    [[nodiscard]] constexpr bool equals_ignoring_ascii_case(StringView other) const
    {
        if (m_length != other.m_length)
            return false;
        for (size_t i = 0; i < m_length; ++i) {
            if (to_ascii_lowercase(static_cast<u8>(m_characters[i])) != to_ascii_lowercase(static_cast<u8>(other.m_characters[i])))
                return false;
        }
        return true;
    }

    [[nodiscard]] constexpr bool starts_with(StringView prefix) const
    {
        return prefix.m_length <= m_length && substring_view(0, prefix.m_length) == prefix;
    }

    [[nodiscard]] constexpr bool starts_with(char character) const { return m_length > 0 && m_characters[0] == character; }

    [[nodiscard]] constexpr bool ends_with(StringView suffix) const
    {
        return suffix.m_length <= m_length && substring_view(m_length - suffix.m_length) == suffix;
    }

    [[nodiscard]] constexpr bool ends_with(char character) const { return m_length > 0 && m_characters[m_length - 1] == character; }

    [[nodiscard]] constexpr Optional<size_t> find(char needle, size_t start = 0) const
    {
        for (size_t i = start; i < m_length; ++i) {
            if (m_characters[i] == needle)
                return i;
        }
        return {};
    }

    [[nodiscard]] constexpr Optional<size_t> find(StringView needle, size_t start = 0) const
    {
        if (needle.m_length > m_length)
            return {};
        for (size_t i = start; i + needle.m_length <= m_length; ++i) {
            if (substring_view(i, needle.m_length) == needle)
                return i;
        }
        return {};
    }

    [[nodiscard]] constexpr Optional<size_t> find_last(char needle) const
    {
        for (size_t i = m_length; i > 0; --i) {
            if (m_characters[i - 1] == needle)
                return i - 1;
        }
        return {};
    }

    [[nodiscard]] constexpr bool contains(char needle) const { return find(needle).has_value(); }
    [[nodiscard]] constexpr bool contains(StringView needle) const { return find(needle).has_value(); }

    [[nodiscard]] constexpr StringView trim_whitespace() const
    {
        size_t start = 0;
        size_t end = m_length;
        while (start < end && is_ascii_space(static_cast<u8>(m_characters[start])))
            ++start;
        while (end > start && is_ascii_space(static_cast<u8>(m_characters[end - 1])))
            --end;
        return substring_view(start, end - start);
    }

    // Parses a decimal integer with an optional sign. Fails on any stray
    // character or on overflow.
    template<Integral T>
    requires(!SameAs<T, bool>)
    [[nodiscard]] constexpr Optional<T> to_number() const
    {
        if (m_length == 0)
            return {};
        size_t index = 0;
        bool negative = false;
        if (m_characters[0] == '+' || m_characters[0] == '-') {
            negative = m_characters[0] == '-';
            if (negative && !IsSigned<T>)
                return {};
            if (++index == m_length)
                return {};
        }
        T value = 0;
        for (; index < m_length; ++index) {
            char character = m_characters[index];
            if (!is_ascii_digit(static_cast<u8>(character)))
                return {};
            T digit = static_cast<T>(character - '0');
            if (__builtin_mul_overflow(value, T(10), &value))
                return {};
            // Accumulating negatives downwards reaches the type's minimum without overflowing.
            bool overflowed = negative ? __builtin_sub_overflow(value, digit, &value) : __builtin_add_overflow(value, digit, &value);
            if (overflowed)
                return {};
        }
        return value;
    }

    // Calls `callback(StringView)` for each piece between separators.
    template<typename Callback>
    constexpr void for_each_split_view(char separator, SplitBehavior behavior, Callback callback) const
    {
        size_t piece_start = 0;
        for (size_t i = 0; i <= m_length; ++i) {
            if (i < m_length && m_characters[i] != separator)
                continue;
            auto piece = substring_view(piece_start, i - piece_start);
            if (!piece.is_empty() || behavior == SplitBehavior::KeepEmpty)
                callback(piece);
            piece_start = i + 1;
        }
    }

private:
    static constexpr size_t length_of(char const* characters)
    {
        size_t length = 0;
        while (characters[length] != '\0')
            ++length;
        return length;
    }

    char const* m_characters { nullptr };
    size_t m_length { 0 };
};

}

[[nodiscard]] constexpr Base::StringView operator""_sv(char const* characters, size_t length)
{
    return { characters, length };
}

using Base::SplitBehavior;
using Base::StringView;
