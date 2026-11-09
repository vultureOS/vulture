/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// What a database holds: one of five kinds of value, whichever a column says it was declared as. Records are how a
// table's rows are written down; keys are how an index's are, so that comparing the bytes compares the values.

namespace Database {

enum class Type : u8 {
    Null,
    Integer,
    Real,
    Text,
    Blob,
};

// What a column would rather hold, from the type it was declared with.
enum class Affinity : u8 {
    Blob,
    Integer,
    Real,
    Numeric,
    Text,
};

class Value {
public:
    Value() = default;

    [[nodiscard]] static Value integer(i64);
    [[nodiscard]] static Value real(double);
    [[nodiscard]] static Value text(StringView);
    [[nodiscard]] static Value blob(ReadonlyBytes);

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool is_null() const { return m_type == Type::Null; }
    [[nodiscard]] bool is_number() const { return m_type == Type::Integer || m_type == Type::Real; }

    // These two are for a value known to be that.
    [[nodiscard]] i64 as_integer() const;
    [[nodiscard]] double as_real() const;
    // The text, or a blob's bytes.
    [[nodiscard]] StringView as_text() const { return m_text.view(); }

    // What SQL makes of it, whatever it is: as much of a text as looks like a number, and so on.
    [[nodiscard]] i64 to_integer() const;
    [[nodiscard]] double to_real() const;
    [[nodiscard]] String to_text() const;
    // True or false, or nothing, for NULL.
    [[nodiscard]] Optional<bool> truth() const;
    // A number for arithmetic: an integer stays one.
    [[nodiscard]] Value to_number() const;

private:
    Type m_type { Type::Null };
    i64 m_integer { 0 };
    double m_real { 0 };
    String m_text;
};

// NULLs, then numbers by value, then text, then blobs, these two by their bytes.
[[nodiscard]] int compare(Value const&, Value const&);

[[nodiscard]] Value apply_affinity(Value, Affinity);
[[nodiscard]] Affinity affinity_of_declared_type(StringView);

void encode_record(Span<Value const>, Vector<u8>& out);
// Into `out`, which is first made empty. Fewer values than a table's columns is a row older than some of them.
ErrorOr<void> decode_record(ReadonlyBytes, Vector<Value>& out);

// Comparing what this appends, byte by byte, is compare() of the values; and a key of several, likewise, column by column.
void encode_key(Value const&, Vector<u8>& out);
// For a table: rowids in the same order as the numbers.
void encode_rowid(i64, Vector<u8>& out);
[[nodiscard]] i64 decode_rowid(ReadonlyBytes);

}
