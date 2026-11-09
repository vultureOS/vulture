/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibBase/NumericLimits.h>
#include <LibDatabase/Platform.h>
#include <LibDatabase/Value.h>

namespace Database {

Value Value::integer(i64 number)
{
    Value value;
    value.m_type = Type::Integer;
    value.m_integer = number;
    return value;
}

Value Value::real(double number)
{
    // There's no such thing as not-a-number in a database: it's a NULL.
    if (number != number)
        return {};
    Value value;
    value.m_type = Type::Real;
    value.m_real = number;
    return value;
}

Value Value::text(StringView text)
{
    Value value;
    value.m_type = Type::Text;
    value.m_text = MUST(String::from_view(text));
    return value;
}

Value Value::blob(ReadonlyBytes bytes)
{
    Value value;
    value.m_type = Type::Blob;
    value.m_text = MUST(String::from_view(StringView(reinterpret_cast<char const*>(bytes.data()), bytes.size())));
    return value;
}

i64 Value::as_integer() const
{
    VERIFY(m_type == Type::Integer);
    return m_integer;
}

double Value::as_real() const
{
    VERIFY(m_type == Type::Real);
    return m_real;
}

// As much of the start of it as is a number, and how much that was. Spaces first are all right.
static size_t numeric_prefix_length(StringView text, bool& is_integer)
{
    size_t index = 0;
    while (index < text.length() && (text[index] == ' ' || (text[index] >= '\t' && text[index] <= '\r')))
        ++index;
    if (index < text.length() && (text[index] == '+' || text[index] == '-'))
        ++index;
    size_t digits = 0;
    while (index < text.length() && text[index] >= '0' && text[index] <= '9') {
        ++index;
        ++digits;
    }
    is_integer = true;
    if (index < text.length() && text[index] == '.') {
        size_t after = index + 1;
        size_t fraction = 0;
        while (after < text.length() && text[after] >= '0' && text[after] <= '9') {
            ++after;
            ++fraction;
        }
        if (digits + fraction > 0) {
            digits += fraction;
            index = after;
            is_integer = false;
        }
    }
    if (digits == 0)
        return 0;
    if (index < text.length() && (text[index] == 'e' || text[index] == 'E')) {
        size_t after = index + 1;
        if (after < text.length() && (text[after] == '+' || text[after] == '-'))
            ++after;
        size_t exponent = 0;
        while (after < text.length() && text[after] >= '0' && text[after] <= '9') {
            ++after;
            ++exponent;
        }
        if (exponent > 0) {
            index = after;
            is_integer = false;
        }
    }
    return index;
}

static double parse_real(StringView text, size_t length)
{
    auto terminated = MUST(String::from_view(text.substring_view(0, length)));
    return strtod(terminated.characters(), nullptr);
}

static i64 integer_from_real(double number)
{
    if (number >= 9223372036854775807.0)
        return NumericLimits<i64>::max();
    if (number <= -9223372036854775808.0)
        return NumericLimits<i64>::min();
    return static_cast<i64>(number);
}

static Optional<i64> parse_integer(StringView text, size_t length)
{
    auto digits = text.substring_view(0, length).trim_whitespace();
    return digits.to_number<i64>();
}

i64 Value::to_integer() const
{
    switch (m_type) {
    case Type::Null:
        return 0;
    case Type::Integer:
        return m_integer;
    case Type::Real:
        return integer_from_real(m_real);
    case Type::Text:
    case Type::Blob: {
        bool is_integer = false;
        size_t length = numeric_prefix_length(m_text.view(), is_integer);
        if (length == 0)
            return 0;
        if (is_integer) {
            if (auto parsed = parse_integer(m_text.view(), length); parsed.has_value())
                return parsed.value();
        }
        return integer_from_real(parse_real(m_text.view(), length));
    }
    }
    VERIFY_NOT_REACHED();
}

double Value::to_real() const
{
    switch (m_type) {
    case Type::Null:
        return 0;
    case Type::Integer:
        return static_cast<double>(m_integer);
    case Type::Real:
        return m_real;
    case Type::Text:
    case Type::Blob: {
        bool is_integer = false;
        size_t length = numeric_prefix_length(m_text.view(), is_integer);
        return length == 0 ? 0 : parse_real(m_text.view(), length);
    }
    }
    VERIFY_NOT_REACHED();
}

static String format_real(double number)
{
    char buffer[40];
    int length = __builtin_snprintf(buffer, sizeof(buffer), "%.15g", number);
    StringView written { buffer, static_cast<size_t>(length) };
    // A real always says so: 2.0, and not 2.
    bool looks_real = false;
    for (char c : written) {
        if (c == '.' || c == 'e' || c == 'n' || c == 'I')
            looks_real = true;
    }
    if (looks_real)
        return MUST(String::from_view(written));
    StringBuilder builder;
    builder.append(written);
    builder.append(".0"_sv);
    return MUST(builder.to_string());
}

String Value::to_text() const
{
    switch (m_type) {
    case Type::Null:
        return {};
    case Type::Integer: {
        StringBuilder builder;
        builder.appendff("{}", m_integer);
        return MUST(builder.to_string());
    }
    case Type::Real:
        return format_real(m_real);
    case Type::Text:
    case Type::Blob:
        return m_text;
    }
    VERIFY_NOT_REACHED();
}

Optional<bool> Value::truth() const
{
    switch (m_type) {
    case Type::Null:
        return {};
    case Type::Integer:
        return m_integer != 0;
    case Type::Real:
        return m_real != 0;
    case Type::Text:
    case Type::Blob:
        return to_real() != 0;
    }
    VERIFY_NOT_REACHED();
}

Value Value::to_number() const
{
    switch (m_type) {
    case Type::Null:
    case Type::Integer:
    case Type::Real:
        return *this;
    case Type::Text:
    case Type::Blob: {
        bool is_integer = false;
        size_t length = numeric_prefix_length(m_text.view(), is_integer);
        if (length == 0)
            return integer(0);
        if (is_integer) {
            if (auto parsed = parse_integer(m_text.view(), length); parsed.has_value())
                return integer(parsed.value());
        }
        return real(parse_real(m_text.view(), length));
    }
    }
    VERIFY_NOT_REACHED();
}

static int compare_bytes(StringView a, StringView b)
{
    size_t common = min(a.length(), b.length());
    int result = common ? __builtin_memcmp(a.characters_without_null_termination(), b.characters_without_null_termination(), common) : 0;
    if (result != 0)
        return result < 0 ? -1 : 1;
    if (a.length() == b.length())
        return 0;
    return a.length() < b.length() ? -1 : 1;
}

// How far an integer is from the nearest double, which is no distance at all until there are more digits than a double has.
static i64 difference_from_rounded(i64 value, double rounded)
{
    // 2^63 is one more than the largest integer there is, and isn't an integer here.
    if (rounded >= 9223372036854775808.0)
        return (value - NumericLimits<i64>::max()) - 1;
    return value - static_cast<i64>(rounded);
}

static int compare_numbers(Value const& a, Value const& b)
{
    if (a.type() == Type::Integer && b.type() == Type::Integer) {
        if (a.as_integer() == b.as_integer())
            return 0;
        return a.as_integer() < b.as_integer() ? -1 : 1;
    }
    // Exactly, even where an integer has more digits than a double does.
    auto as_exact = [](Value const& value, double& rounded, i64& difference) {
        if (value.type() == Type::Integer) {
            rounded = static_cast<double>(value.as_integer());
            difference = difference_from_rounded(value.as_integer(), rounded);
        } else {
            rounded = value.as_real();
            difference = 0;
        }
    };
    double a_rounded, b_rounded;
    i64 a_difference, b_difference;
    as_exact(a, a_rounded, a_difference);
    as_exact(b, b_rounded, b_difference);
    if (a_rounded != b_rounded)
        return a_rounded < b_rounded ? -1 : 1;
    if (a_difference == b_difference)
        return 0;
    return a_difference < b_difference ? -1 : 1;
}

static int type_order(Type type)
{
    switch (type) {
    case Type::Null:
        return 0;
    case Type::Integer:
    case Type::Real:
        return 1;
    case Type::Text:
        return 2;
    case Type::Blob:
        return 3;
    }
    VERIFY_NOT_REACHED();
}

int compare(Value const& a, Value const& b)
{
    int a_order = type_order(a.type());
    int b_order = type_order(b.type());
    if (a_order != b_order)
        return a_order < b_order ? -1 : 1;
    switch (a_order) {
    case 0:
        return 0;
    case 1:
        return compare_numbers(a, b);
    default:
        return compare_bytes(a.as_text(), b.as_text());
    }
}

// All of it must be a number, for a column that would rather hold one to take it as one.
static Optional<Value> text_as_number(StringView text)
{
    bool is_integer = false;
    size_t length = numeric_prefix_length(text, is_integer);
    if (length == 0)
        return {};
    for (size_t i = length; i < text.length(); ++i) {
        if (!(text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r')))
            return {};
    }
    if (is_integer) {
        if (auto parsed = parse_integer(text, length); parsed.has_value())
            return Value::integer(parsed.value());
    }
    return Value::real(parse_real(text, length));
}

static bool real_is_exactly_integer(double number, i64& integer)
{
    if (number < -9223372036854775808.0 || number >= 9223372036854775808.0)
        return false;
    integer = static_cast<i64>(number);
    return static_cast<double>(integer) == number;
}

Value apply_affinity(Value value, Affinity affinity)
{
    switch (affinity) {
    case Affinity::Blob:
        return value;
    case Affinity::Text:
        if (value.is_number())
            return Value::text(value.to_text().view());
        return value;
    case Affinity::Real:
        if (value.type() == Type::Integer)
            return Value::real(static_cast<double>(value.as_integer()));
        if (value.type() == Type::Text) {
            if (auto number = text_as_number(value.as_text()); number.has_value())
                return Value::real(number->to_real());
        }
        return value;
    case Affinity::Integer:
    case Affinity::Numeric: {
        Value number = value;
        if (value.type() == Type::Text) {
            auto parsed = text_as_number(value.as_text());
            if (!parsed.has_value())
                return value;
            number = parsed.release_value();
        }
        if (number.type() == Type::Real) {
            i64 integer = 0;
            if (real_is_exactly_integer(number.as_real(), integer))
                return Value::integer(integer);
        }
        return number;
    }
    }
    VERIFY_NOT_REACHED();
}

Affinity affinity_of_declared_type(StringView declared)
{
    auto has = [&](StringView word) {
        for (size_t i = 0; i + word.length() <= declared.length(); ++i) {
            if (declared.substring_view(i, word.length()).equals_ignoring_ascii_case(word))
                return true;
        }
        return false;
    };
    if (has("INT"_sv))
        return Affinity::Integer;
    if (has("CHAR"_sv) || has("CLOB"_sv) || has("TEXT"_sv))
        return Affinity::Text;
    if (declared.is_empty() || has("BLOB"_sv))
        return Affinity::Blob;
    if (has("REAL"_sv) || has("FLOA"_sv) || has("DOUB"_sv))
        return Affinity::Real;
    return Affinity::Numeric;
}

// ---- Records.

namespace {

enum Tag : u8 {
    TagNull,
    TagZero,
    TagOne,
    TagInteger8,
    TagInteger16,
    TagInteger32,
    TagInteger64,
    TagReal,
    TagText,
    TagBlob,
};

void append_varint(Vector<u8>& out, u64 number)
{
    while (number >= 0x80) {
        out.append(static_cast<u8>(number | 0x80));
        number >>= 7;
    }
    out.append(static_cast<u8>(number));
}

void append_little_endian(Vector<u8>& out, u64 number, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
        out.append(static_cast<u8>(number >> (i * 8)));
}

}

void encode_record(Span<Value const> values, Vector<u8>& out)
{
    append_varint(out, values.size());
    for (auto const& value : values) {
        switch (value.type()) {
        case Type::Null:
            out.append(TagNull);
            break;
        case Type::Integer: {
            i64 number = value.as_integer();
            if (number == 0) {
                out.append(TagZero);
            } else if (number == 1) {
                out.append(TagOne);
            } else if (number >= -128 && number <= 127) {
                out.append(TagInteger8);
                append_little_endian(out, static_cast<u64>(number), 1);
            } else if (number >= -32768 && number <= 32767) {
                out.append(TagInteger16);
                append_little_endian(out, static_cast<u64>(number), 2);
            } else if (number >= -2147483648ll && number <= 2147483647ll) {
                out.append(TagInteger32);
                append_little_endian(out, static_cast<u64>(number), 4);
            } else {
                out.append(TagInteger64);
                append_little_endian(out, static_cast<u64>(number), 8);
            }
            break;
        }
        case Type::Real: {
            out.append(TagReal);
            double number = value.as_real();
            u64 bits;
            __builtin_memcpy(&bits, &number, sizeof(bits));
            append_little_endian(out, bits, 8);
            break;
        }
        case Type::Text:
        case Type::Blob: {
            out.append(value.type() == Type::Text ? TagText : TagBlob);
            auto text = value.as_text();
            append_varint(out, text.length());
            for (char c : text)
                out.append(static_cast<u8>(c));
            break;
        }
        }
    }
}

namespace {

constexpr Error corrupt_record = Error::from_string_literal("database: a record is damaged");

struct Reader {
    ReadonlyBytes bytes;
    size_t position { 0 };

    ErrorOr<u64> read_varint()
    {
        u64 number = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (position >= bytes.size())
                return corrupt_record;
            u8 byte = bytes[position++];
            number |= static_cast<u64>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return number;
        }
        return corrupt_record;
    }

    ErrorOr<u64> read_little_endian(size_t count)
    {
        if (position + count > bytes.size())
            return corrupt_record;
        u64 number = 0;
        for (size_t i = 0; i < count; ++i)
            number |= static_cast<u64>(bytes[position + i]) << (i * 8);
        position += count;
        return number;
    }
};

i64 sign_extended(u64 number, unsigned bits)
{
    u64 const sign = 1ull << (bits - 1);
    return static_cast<i64>((number ^ sign) - sign);
}

}

ErrorOr<void> decode_record(ReadonlyBytes bytes, Vector<Value>& out)
{
    out.clear();
    Reader reader { bytes };
    u64 count = TRY(reader.read_varint());
    for (u64 i = 0; i < count; ++i) {
        if (reader.position >= bytes.size())
            return corrupt_record;
        u8 tag = bytes[reader.position++];
        switch (tag) {
        case TagNull:
            out.append(Value {});
            break;
        case TagZero:
            out.append(Value::integer(0));
            break;
        case TagOne:
            out.append(Value::integer(1));
            break;
        case TagInteger8:
            out.append(Value::integer(sign_extended(TRY(reader.read_little_endian(1)), 8)));
            break;
        case TagInteger16:
            out.append(Value::integer(sign_extended(TRY(reader.read_little_endian(2)), 16)));
            break;
        case TagInteger32:
            out.append(Value::integer(sign_extended(TRY(reader.read_little_endian(4)), 32)));
            break;
        case TagInteger64:
            out.append(Value::integer(static_cast<i64>(TRY(reader.read_little_endian(8)))));
            break;
        case TagReal: {
            u64 bits = TRY(reader.read_little_endian(8));
            double number;
            __builtin_memcpy(&number, &bits, sizeof(number));
            out.append(Value::real(number));
            break;
        }
        case TagText:
        case TagBlob: {
            u64 length = TRY(reader.read_varint());
            if (reader.position + length > bytes.size())
                return corrupt_record;
            auto piece = ReadonlyBytes(bytes.data() + reader.position, static_cast<size_t>(length));
            reader.position += static_cast<size_t>(length);
            if (tag == TagText)
                out.append(Value::text(StringView(reinterpret_cast<char const*>(piece.data()), piece.size())));
            else
                out.append(Value::blob(piece));
            break;
        }
        default:
            return corrupt_record;
        }
    }
    return {};
}

// ---- Keys.

static void append_big_endian(Vector<u8>& out, u64 number, size_t bytes)
{
    for (size_t i = bytes; i > 0; --i)
        out.append(static_cast<u8>(number >> ((i - 1) * 8)));
}

static void encode_number(double rounded, i64 difference, Vector<u8>& out)
{
    if (rounded == 0)
        rounded = 0; // Negative zero is zero.
    u64 bits;
    __builtin_memcpy(&bits, &rounded, sizeof(bits));
    // Negative numbers, largest first, come before positive ones, smallest first.
    bits = (bits >> 63) ? ~bits : (bits | (1ull << 63));
    append_big_endian(out, bits, 8);
    append_big_endian(out, static_cast<u64>(difference + 0x4000'0000), 4);
}

static void encode_bytes(StringView text, Vector<u8>& out)
{
    // A zero inside is 00 FF; the end is 00 01, which comes before any of that and before anything else.
    for (char c : text) {
        out.append(static_cast<u8>(c));
        if (c == 0)
            out.append(0xff);
    }
    out.append(0);
    out.append(1);
}

void encode_key(Value const& value, Vector<u8>& out)
{
    switch (value.type()) {
    case Type::Null:
        out.append(1);
        break;
    case Type::Integer: {
        out.append(2);
        double rounded = static_cast<double>(value.as_integer());
        encode_number(rounded, difference_from_rounded(value.as_integer(), rounded), out);
        break;
    }
    case Type::Real:
        out.append(2);
        encode_number(value.as_real(), 0, out);
        break;
    case Type::Text:
        out.append(3);
        encode_bytes(value.as_text(), out);
        break;
    case Type::Blob:
        out.append(4);
        encode_bytes(value.as_text(), out);
        break;
    }
}

void encode_rowid(i64 rowid, Vector<u8>& out)
{
    append_big_endian(out, static_cast<u64>(rowid) ^ (1ull << 63), 8);
}

i64 decode_rowid(ReadonlyBytes bytes)
{
    VERIFY(bytes.size() >= 8);
    u64 number = 0;
    for (size_t i = 0; i < 8; ++i)
        number = (number << 8) | bytes[i];
    return static_cast<i64>(number ^ (1ull << 63));
}

}
