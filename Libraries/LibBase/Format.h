/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/StringView.h>

// Type-safe formatting with "{}" placeholders:
//
//     format_to(sink, "{} has {:#x} bytes", name, size);
//
// Spec: {[:[[fill]align][sign]['#']['0'][width][type]]}
//   align  '<' left, '>' right, '^' center
//   sign   '+' also shows a sign for non-negative numbers
//   '#'    prefix: 0x / 0X / 0b / 0o
//   '0'    pad numbers with zeros after the sign and prefix
//   type   d (decimal), x / X (hex), b (binary), o (octal), c (integer as character)
//
// "{{" and "}}" produce literal braces. The number of placeholders is
// checked against the number of arguments at compile time.

namespace Base {

// Receives formatted output. Implementations are usually `final`.
class FormatSink {
public:
    virtual void write(StringView) = 0;

protected:
    ~FormatSink() = default;
};

struct FormatSpec {
    enum class Align : u8 {
        Default,
        Left,
        Right,
        Center,
    };

    enum class Sign : u8 {
        NegativeOnly,
        Always,
    };

    char fill { ' ' };
    Align align { Align::Default };
    Sign sign { Sign::NegativeOnly };
    bool alternate_form { false };
    bool zero_pad { false };
    size_t width { 0 };
    char type { 0 };
};

// Writes `content`, padded to `spec.width` with `spec.fill`.
void write_padded(FormatSink&, FormatSpec const&, StringView content, FormatSpec::Align default_align);

void format_integer(FormatSink&, FormatSpec const&, u64 magnitude, bool is_negative);

// Specialize Formatter<T> with `static void format(FormatSink&, FormatSpec const&, T const&)`
// to make T formattable.
template<typename T>
struct Formatter;

template<typename T>
concept HasFormatter = requires(FormatSink& sink, FormatSpec const& spec, T const& value) {
    Formatter<T>::format(sink, spec, value);
};

template<Integral T>
requires(!SameAs<T, bool> && !SameAs<T, char>)
struct Formatter<T> {
    static void format(FormatSink& sink, FormatSpec const& spec, T value)
    {
        if constexpr (IsSigned<T>) {
            if (value < 0) {
                format_integer(sink, spec, u64(0) - static_cast<u64>(value), true);
                return;
            }
        }
        format_integer(sink, spec, static_cast<u64>(value), false);
    }
};

template<Enum T>
struct Formatter<T> {
    static void format(FormatSink& sink, FormatSpec const& spec, T value)
    {
        Formatter<UnderlyingType<T>>::format(sink, spec, static_cast<UnderlyingType<T>>(value));
    }
};

template<>
struct Formatter<bool> {
    static void format(FormatSink&, FormatSpec const&, bool);
};

template<>
struct Formatter<char> {
    static void format(FormatSink&, FormatSpec const&, char);
};

template<>
struct Formatter<StringView> {
    static void format(FormatSink&, FormatSpec const&, StringView);
};

template<>
struct Formatter<char const*> {
    static void format(FormatSink&, FormatSpec const&, char const*);
};

template<>
struct Formatter<char*> : Formatter<char const*> { };

template<size_t N>
struct Formatter<char[N]> {
    static void format(FormatSink& sink, FormatSpec const& spec, char const (&characters)[N])
    {
        // A char array may hold a shorter, null-terminated string.
        size_t length = 0;
        while (length < N && characters[length] != '\0')
            ++length;
        Formatter<StringView>::format(sink, spec, StringView(characters, length));
    }
};

template<>
struct Formatter<nullptr_t> {
    static void format(FormatSink&, FormatSpec const&, nullptr_t);
};

void format_pointer(FormatSink&, FormatSpec const&, FlatPtr);

template<typename T>
struct Formatter<T*> {
    static void format(FormatSink& sink, FormatSpec const& spec, T const* pointer)
    {
        format_pointer(sink, spec, reinterpret_cast<FlatPtr>(pointer));
    }
};

template<typename T>
requires(HasFormatter<T>)
struct Formatter<Optional<T>> {
    static void format(FormatSink& sink, FormatSpec const& spec, Optional<T> const& optional)
    {
        if (!optional.has_value()) {
            write_padded(sink, spec, "None"_sv, FormatSpec::Align::Left);
            return;
        }
        Formatter<T>::format(sink, spec, optional.value());
    }
};

// A format argument with its type erased, so vformat() is compiled once.
struct FormatArgument {
    void const* value;
    void (*format)(FormatSink&, FormatSpec const&, void const*);
};

template<typename T>
constexpr FormatArgument make_format_argument(T const& value)
{
    return { &value, [](FormatSink& sink, FormatSpec const& spec, void const* erased) {
                Formatter<T>::format(sink, spec, *static_cast<T const*>(erased));
            } };
}

// Formats a string checked earlier; prefer format_to().
void vformat(FormatSink&, StringView format, Span<FormatArgument const> arguments);

namespace Detail {

// Never defined. A compile-time format check that fails calls one of these,
// and the compiler names it in the error.
void format_string_has_unbalanced_braces();
void format_string_placeholder_count_does_not_match_argument_count();

consteval size_t count_format_placeholders(char const* format, size_t length)
{
    size_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        if (format[i] == '{') {
            if (i + 1 < length && format[i + 1] == '{') {
                ++i;
                continue;
            }
            size_t close = i + 1;
            while (close < length && format[close] != '}' && format[close] != '{')
                ++close;
            if (close == length || format[close] == '{')
                format_string_has_unbalanced_braces();
            ++count;
            i = close;
        } else if (format[i] == '}') {
            if (i + 1 < length && format[i + 1] == '}') {
                ++i;
                continue;
            }
            format_string_has_unbalanced_braces();
        }
    }
    return count;
}

}

template<typename... Args>
struct CheckedFormatString {
    template<size_t N>
    consteval CheckedFormatString(char const (&format)[N])
        : view(format, N - 1)
    {
        if (Detail::count_format_placeholders(format, N - 1) != sizeof...(Args))
            Detail::format_string_placeholder_count_does_not_match_argument_count();
    }

    StringView view;
};

template<typename... Args>
void format_to(FormatSink& sink, CheckedFormatString<TypeIdentity<Args>...> format, Args const&... args)
{
    // The trailing empty argument keeps the array non-empty when there are no arguments.
    FormatArgument const arguments[] { make_format_argument(args)..., FormatArgument {} };
    vformat(sink, format.view, Span<FormatArgument const>(arguments, sizeof...(Args)));
}

// Formats into a fixed buffer, truncating once it is full. The contents stay
// null-terminated for APIs that need it.
template<size_t Capacity>
class FixedStringBuilder final : public FormatSink {
public:
    void write(StringView text) override
    {
        size_t count = min(text.length(), Capacity - m_length);
        for (size_t i = 0; i < count; ++i)
            m_buffer[m_length + i] = text[i];
        m_length += count;
        m_buffer[m_length] = '\0';
        if (count < text.length())
            m_truncated = true;
    }

    void append(StringView text) { write(text); }

    template<typename... Args>
    void append_formatted(CheckedFormatString<TypeIdentity<Args>...> format, Args const&... args)
    {
        format_to(*this, format, args...);
    }

    void clear()
    {
        m_length = 0;
        m_buffer[0] = '\0';
        m_truncated = false;
    }

    [[nodiscard]] StringView view() const { return { m_buffer, m_length }; }
    [[nodiscard]] char const* null_terminated() const { return m_buffer; }
    [[nodiscard]] size_t length() const { return m_length; }
    [[nodiscard]] bool is_truncated() const { return m_truncated; }

private:
    char m_buffer[Capacity + 1] { '\0' };
    size_t m_length { 0 };
    bool m_truncated { false };
};

}

using Base::CheckedFormatString;
using Base::FixedStringBuilder;
using Base::format_to;
using Base::FormatSink;
