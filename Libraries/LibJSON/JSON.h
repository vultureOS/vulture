/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Optional.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// JSON (RFC 8259): a value, a parser and a writer. Objects keep their
// members in the order they were given, so files read and written back
// keep theirs. This is the format of vultureOS's property lists (launchd
// plists, preferences, themes).

namespace JSON {

class Value {
public:
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Value() = default;
    Value(bool value)
        : m_type(Type::Boolean)
        , m_boolean(value)
    {
    }
    Value(int value)
        : Value(static_cast<i64>(value))
    {
    }
    Value(i64 value)
        : m_type(Type::Number)
        , m_number(static_cast<double>(value))
        , m_integer(value)
        , m_is_integer(true)
    {
    }
    Value(double value)
        : m_type(Type::Number)
        , m_number(value)
    {
    }
    [[nodiscard]] static ErrorOr<Value> string(StringView);
    [[nodiscard]] static Value array() { return Value(Type::Array); }
    [[nodiscard]] static Value object() { return Value(Type::Object); }

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool is_null() const { return m_type == Type::Null; }
    [[nodiscard]] bool is_boolean() const { return m_type == Type::Boolean; }
    [[nodiscard]] bool is_number() const { return m_type == Type::Number; }
    // A number written without fraction or exponent, that fits in an i64.
    [[nodiscard]] bool is_integer() const { return m_type == Type::Number && m_is_integer; }
    [[nodiscard]] bool is_string() const { return m_type == Type::String; }
    [[nodiscard]] bool is_array() const { return m_type == Type::Array; }
    [[nodiscard]] bool is_object() const { return m_type == Type::Object; }

    [[nodiscard]] bool as_boolean() const
    {
        VERIFY(is_boolean());
        return m_boolean;
    }
    [[nodiscard]] double as_number() const
    {
        VERIFY(is_number());
        return m_number;
    }
    [[nodiscard]] i64 as_integer() const
    {
        VERIFY(is_integer());
        return m_integer;
    }
    [[nodiscard]] StringView as_string() const
    {
        VERIFY(is_string());
        return m_string.view();
    }

    // Arrays: the elements; objects: the values, in order.
    [[nodiscard]] size_t size() const { return m_elements.size(); }
    [[nodiscard]] Value const& at(size_t index) const { return m_elements[index]; }
    ErrorOr<void> append(Value);

    // Objects.
    [[nodiscard]] StringView key_at(size_t index) const { return m_keys[index].view(); }
    [[nodiscard]] Value const* get(StringView key) const;
    [[nodiscard]] bool has(StringView key) const { return get(key) != nullptr; }
    // Replaces the value of a member that is there already.
    ErrorOr<void> set(StringView key, Value);
    bool remove(StringView key);

    // The member, if it is there and of that type.
    [[nodiscard]] Optional<StringView> get_string(StringView key) const;
    [[nodiscard]] Optional<bool> get_boolean(StringView key) const;
    [[nodiscard]] Optional<i64> get_integer(StringView key) const;
    [[nodiscard]] Optional<double> get_number(StringView key) const;
    [[nodiscard]] Value const* get_array(StringView key) const;
    [[nodiscard]] Value const* get_object(StringView key) const;

    // Compact, or indented by four spaces per level (as macOS's plutil
    // -convert json -r writes, with "key" : value).
    [[nodiscard]] ErrorOr<String> serialize(bool pretty = false) const;

    bool operator==(Value const&) const;

private:
    explicit Value(Type type)
        : m_type(type)
    {
    }

    ErrorOr<void> serialize_into(StringBuilder&, bool pretty, size_t depth) const;

    Type m_type { Type::Null };
    bool m_boolean { false };
    double m_number { 0 };
    i64 m_integer { 0 };
    bool m_is_integer { false };
    String m_string;
    Vector<String> m_keys;
    Vector<Value> m_elements;
};

// Where and why parsing stopped.
struct ParseError {
    size_t line { 1 };
    size_t column { 1 };
    char const* message { "" };
};

// The one value `text` holds (whitespace around it allowed). Nesting deeper
// than 256 levels is refused.
ErrorOr<Value> parse(StringView text, ParseError* = nullptr);

}
