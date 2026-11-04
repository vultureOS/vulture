/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Format.h>
#include <LibBase/New.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// An owned, immutable string (always NUL-terminated, for convenience at
// C boundaries), and a builder to make one piece by piece.

namespace Base {

class String {
public:
    String() = default;

    [[nodiscard]] static ErrorOr<String> from_view(StringView view)
    {
        String string;
        if (view.is_empty())
            return string;
        auto* characters = static_cast<char*>(operator new(view.length() + 1, nothrow));
        if (!characters)
            return Error::from_errno(12);
        __builtin_memcpy(characters, view.characters_without_null_termination(), view.length());
        characters[view.length()] = '\0';
        string.m_characters = characters;
        string.m_length = view.length();
        return string;
    }

    String(String&& other)
        : m_characters(exchange(other.m_characters, nullptr))
        , m_length(exchange(other.m_length, 0))
    {
    }

    String(String const& other)
        : String(MUST(from_view(other.view())))
    {
    }

    ~String() { operator delete(m_characters); }

    String& operator=(String&& other)
    {
        if (this != &other) {
            operator delete(m_characters);
            m_characters = exchange(other.m_characters, nullptr);
            m_length = exchange(other.m_length, 0);
        }
        return *this;
    }

    String& operator=(String const& other)
    {
        if (this != &other)
            *this = String(other);
        return *this;
    }

    [[nodiscard]] StringView view() const { return m_characters ? StringView(m_characters, m_length) : StringView(); }
    [[nodiscard]] size_t length() const { return m_length; }
    [[nodiscard]] bool is_empty() const { return m_length == 0; }
    // NUL-terminated; "" for an empty string.
    [[nodiscard]] char const* characters() const { return m_characters ? m_characters : ""; }

    bool operator==(String const& other) const { return view() == other.view(); }
    bool operator==(StringView other) const { return view() == other; }

private:
    char* m_characters { nullptr };
    size_t m_length { 0 };
};

class StringBuilder final : public FormatSink {
public:
    void write(StringView text) override
    {
        if (m_failed)
            return;
        for (char c : text) {
            if (m_buffer.try_append(c).is_error()) {
                m_failed = true;
                return;
            }
        }
    }

    void append(StringView text) { write(text); }
    void append(char c) { write({ &c, 1 }); }
    // As UTF-8.
    void append_code_point(u32 code)
    {
        if (code < 0x80) {
            append(static_cast<char>(code));
        } else if (code < 0x800) {
            append(static_cast<char>(0xc0 | (code >> 6)));
            append(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            append(static_cast<char>(0xe0 | (code >> 12)));
            append(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            append(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            append(static_cast<char>(0xf0 | (code >> 18)));
            append(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            append(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            append(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    template<typename... Args>
    void appendff(CheckedFormatString<TypeIdentity<Args>...> format, Args const&... args)
    {
        format_to(*this, format, args...);
    }

    [[nodiscard]] StringView view() const { return { m_buffer.data(), m_buffer.size() }; }
    [[nodiscard]] size_t length() const { return m_buffer.size(); }
    void clear() { m_buffer.clear(); }

    [[nodiscard]] ErrorOr<String> to_string() const
    {
        if (m_failed)
            return Error::from_errno(12);
        return String::from_view(view());
    }

private:
    Vector<char> m_buffer;
    bool m_failed { false };
};

template<>
struct Formatter<String> {
    static void format(FormatSink& sink, FormatSpec const& spec, String const& string)
    {
        Formatter<StringView>::format(sink, spec, string.view());
    }
};

}

using Base::String;
using Base::StringBuilder;
