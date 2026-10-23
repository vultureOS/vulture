/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Format.h>
#include <LibBase/StringView.h>

namespace Base {

// An error is either an errno-style code or a static message.
class Error {
public:
    [[nodiscard]] static constexpr Error from_errno(int code) { return Error(code, {}); }

    template<size_t N>
    [[nodiscard]] static constexpr Error from_string_literal(char const (&message)[N])
    {
        return Error(0, StringView(message, N - 1));
    }

    [[nodiscard]] constexpr int code() const { return m_code; }
    [[nodiscard]] constexpr bool is_errno() const { return m_code != 0; }
    [[nodiscard]] constexpr StringView string_literal() const { return m_string_literal; }

    constexpr bool operator==(Error const&) const = default;

private:
    constexpr Error(int code, StringView string_literal)
        : m_code(code)
        , m_string_literal(string_literal)
    {
    }

    int m_code { 0 };
    StringView m_string_literal;
};

// Either a T or an error. Use TRY() to propagate errors and MUST() where
// failure is a bug.
template<typename T, typename E = Error>
class [[nodiscard]] ErrorOr {
    static_assert(!IsLvalueReference<T>, "ErrorOr of references is not supported");

public:
    using ValueType = T;
    using ErrorType = E;

    template<typename U>
    requires(ConstructibleFrom<T, U &&> && !SameAs<RemoveCVReference<U>, ErrorOr> && !SameAs<RemoveCVReference<U>, E>)
    constexpr ErrorOr(U&& value)
        : m_value(forward<U>(value))
        , m_is_error(false)
    {
    }

    constexpr ErrorOr(E error)
        : m_error(move(error))
        , m_is_error(true)
    {
    }

    ErrorOr(ErrorOr&& other)
        : m_is_error(other.m_is_error)
    {
        if (m_is_error)
            new (__builtin_addressof(m_error)) E(move(other.m_error));
        else
            new (__builtin_addressof(m_value)) T(move(other.m_value));
    }

    ErrorOr(ErrorOr const& other) requires(IsConstructible<T, T const&> && IsConstructible<E, E const&>)
        : m_is_error(other.m_is_error)
    {
        if (m_is_error)
            new (__builtin_addressof(m_error)) E(other.m_error);
        else
            new (__builtin_addressof(m_value)) T(other.m_value);
    }

    ErrorOr& operator=(ErrorOr&& other)
    {
        if (this != &other) {
            destroy();
            new (this) ErrorOr(move(other));
        }
        return *this;
    }

    ErrorOr& operator=(ErrorOr const& other) requires(IsConstructible<T, T const&> && IsConstructible<E, E const&>)
    {
        if (this != &other) {
            destroy();
            new (this) ErrorOr(other);
        }
        return *this;
    }

    ~ErrorOr() { destroy(); }

    [[nodiscard]] constexpr bool is_error() const { return m_is_error; }

    [[nodiscard]] constexpr T& value()
    {
        VERIFY(!m_is_error);
        return m_value;
    }

    [[nodiscard]] constexpr T const& value() const
    {
        VERIFY(!m_is_error);
        return m_value;
    }

    [[nodiscard]] constexpr E& error()
    {
        VERIFY(m_is_error);
        return m_error;
    }

    [[nodiscard]] constexpr E const& error() const
    {
        VERIFY(m_is_error);
        return m_error;
    }

    [[nodiscard]] constexpr T release_value()
    {
        VERIFY(!m_is_error);
        return move(m_value);
    }

    [[nodiscard]] constexpr E release_error()
    {
        VERIFY(m_is_error);
        return move(m_error);
    }

private:
    void destroy()
    {
        if (m_is_error)
            m_error.~E();
        else
            m_value.~T();
    }

    union {
        T m_value;
        E m_error;
    };
    bool m_is_error;
};

template<typename E>
class [[nodiscard]] ErrorOr<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    constexpr ErrorOr() = default;

    constexpr ErrorOr(E error)
        : m_error(move(error))
    {
    }

    [[nodiscard]] constexpr bool is_error() const { return m_error.has_value(); }
    [[nodiscard]] constexpr E& error() { return m_error.value(); }
    [[nodiscard]] constexpr E const& error() const { return m_error.value(); }
    [[nodiscard]] constexpr E release_error() { return m_error.release_value(); }
    constexpr void release_value() { VERIFY(!is_error()); }

private:
    Optional<E> m_error;
};

template<>
struct Formatter<Error> {
    static void format(FormatSink& sink, FormatSpec const& spec, Error const& error)
    {
        if (!error.is_errno()) {
            write_padded(sink, spec, error.string_literal(), FormatSpec::Align::Left);
            return;
        }
        FixedStringBuilder<32> text;
        format_to(text, "errno {}", error.code());
        write_padded(sink, spec, text.view(), FormatSpec::Align::Left);
    }
};

}

using Base::Error;
using Base::ErrorOr;

#define BASE_CONCATENATE_IMPL(a, b) a##b
#define BASE_CONCATENATE(a, b) BASE_CONCATENATE_IMPL(a, b)

// Evaluates an ErrorOr expression; on error, returns the error from the
// enclosing function, otherwise yields the value.
#define TRY(expression) TRY_IMPL(expression, BASE_CONCATENATE(try_result_, __COUNTER__))
#define TRY_IMPL(expression, result)        \
    ({                                      \
        auto&& result = (expression);       \
        if (result.is_error()) [[unlikely]] \
            return result.release_error();  \
        result.release_value();             \
    })

// Like TRY, but an error is a bug.
#define MUST(expression) MUST_IMPL(expression, BASE_CONCATENATE(must_result_, __COUNTER__))
#define MUST_IMPL(expression, result) \
    ({                                \
        auto&& result = (expression); \
        VERIFY(!result.is_error());   \
        result.release_value();       \
    })
