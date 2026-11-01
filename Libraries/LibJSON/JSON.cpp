/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibJSON/JSON.h>

namespace JSON {

namespace {

constexpr int error_invalid = 22; // EINVAL
constexpr size_t max_depth = 256;

class Parser {
public:
    Parser(StringView text, ParseError* error)
        : m_text(text)
        , m_error(error)
    {
    }

    ErrorOr<Value> parse_document()
    {
        skip_whitespace();
        auto value = TRY(parse_value(0));
        skip_whitespace();
        if (m_position < m_text.length())
            return fail("unexpected text after the value");
        return value;
    }

private:
    Error fail(char const* message)
    {
        if (m_error) {
            m_error->line = 1;
            m_error->column = 1;
            for (size_t i = 0; i < m_position && i < m_text.length(); ++i) {
                if (m_text[i] == '\n') {
                    ++m_error->line;
                    m_error->column = 1;
                } else if ((static_cast<u8>(m_text[i]) & 0xc0) != 0x80) {
                    ++m_error->column;
                }
            }
            m_error->message = message;
        }
        return Error::from_errno(error_invalid);
    }

    [[nodiscard]] bool at_end() const { return m_position >= m_text.length(); }
    [[nodiscard]] char peek() const { return at_end() ? '\0' : m_text[m_position]; }

    void skip_whitespace()
    {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
            ++m_position;
    }

    bool consume_literal(StringView literal)
    {
        if (m_text.substring_view(m_position).starts_with(literal)) {
            m_position += literal.length();
            return true;
        }
        return false;
    }

    ErrorOr<Value> parse_value(size_t depth)
    {
        if (depth > max_depth)
            return fail("nested too deeply");
        switch (peek()) {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"': {
            StringBuilder builder;
            TRY(parse_string(builder));
            return Value::string(builder.view());
        }
        case 't':
            if (consume_literal("true"_sv))
                return Value(true);
            break;
        case 'f':
            if (consume_literal("false"_sv))
                return Value(false);
            break;
        case 'n':
            if (consume_literal("null"_sv))
                return Value();
            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                return parse_number();
            break;
        }
        return fail(at_end() ? "expected a value, found the end" : "expected a value");
    }

    ErrorOr<Value> parse_object(size_t depth)
    {
        ++m_position; // {
        auto object = Value::object();
        skip_whitespace();
        if (peek() == '}') {
            ++m_position;
            return object;
        }
        for (;;) {
            skip_whitespace();
            if (peek() != '"')
                return fail("expected a member name in quotes");
            StringBuilder key;
            TRY(parse_string(key));
            skip_whitespace();
            if (peek() != ':')
                return fail("expected ':' after the member name");
            ++m_position;
            skip_whitespace();
            auto value = TRY(parse_value(depth + 1));
            TRY(object.set(key.view(), move(value)));
            skip_whitespace();
            if (peek() == ',') {
                ++m_position;
                continue;
            }
            if (peek() == '}') {
                ++m_position;
                return object;
            }
            return fail("expected ',' or '}' in the object");
        }
    }

    ErrorOr<Value> parse_array(size_t depth)
    {
        ++m_position; // [
        auto array = Value::array();
        skip_whitespace();
        if (peek() == ']') {
            ++m_position;
            return array;
        }
        for (;;) {
            skip_whitespace();
            TRY(array.append(TRY(parse_value(depth + 1))));
            skip_whitespace();
            if (peek() == ',') {
                ++m_position;
                continue;
            }
            if (peek() == ']') {
                ++m_position;
                return array;
            }
            return fail("expected ',' or ']' in the array");
        }
    }

    static void append_utf8(StringBuilder& builder, u32 code_point)
    {
        if (code_point < 0x80) {
            builder.append(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            builder.append(static_cast<char>(0xc0 | (code_point >> 6)));
            builder.append(static_cast<char>(0x80 | (code_point & 0x3f)));
        } else if (code_point < 0x10000) {
            builder.append(static_cast<char>(0xe0 | (code_point >> 12)));
            builder.append(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
            builder.append(static_cast<char>(0x80 | (code_point & 0x3f)));
        } else {
            builder.append(static_cast<char>(0xf0 | (code_point >> 18)));
            builder.append(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
            builder.append(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
            builder.append(static_cast<char>(0x80 | (code_point & 0x3f)));
        }
    }

    ErrorOr<u32> parse_hex4()
    {
        u32 value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = peek();
            u32 digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<u32>(c - 'A' + 10);
            else
                return fail("expected four hexadecimal digits after \\u");
            value = value * 16 + digit;
            ++m_position;
        }
        return value;
    }

    ErrorOr<void> parse_string(StringBuilder& builder)
    {
        ++m_position; // "
        for (;;) {
            if (at_end())
                return fail("unterminated string");
            char c = m_text[m_position];
            if (c == '"') {
                ++m_position;
                return {};
            }
            if (static_cast<u8>(c) < 0x20)
                return fail("control character in a string");
            if (c != '\\') {
                builder.append(c);
                ++m_position;
                continue;
            }
            ++m_position;
            char escape = peek();
            ++m_position;
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                builder.append(escape);
                break;
            case 'b':
                builder.append('\b');
                break;
            case 'f':
                builder.append('\f');
                break;
            case 'n':
                builder.append('\n');
                break;
            case 'r':
                builder.append('\r');
                break;
            case 't':
                builder.append('\t');
                break;
            case 'u': {
                u32 code_point = TRY(parse_hex4());
                // A surrogate pair makes one code point beyond the BMP.
                if (code_point >= 0xd800 && code_point < 0xdc00) {
                    if (!consume_literal("\\u"_sv))
                        return fail("a high surrogate without its low one");
                    u32 low = TRY(parse_hex4());
                    if (low < 0xdc00 || low >= 0xe000)
                        return fail("a high surrogate without its low one");
                    code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
                } else if (code_point >= 0xdc00 && code_point < 0xe000) {
                    return fail("a low surrogate on its own");
                }
                append_utf8(builder, code_point);
                break;
            }
            default:
                --m_position;
                return fail("unknown escape in a string");
            }
        }
    }

    ErrorOr<Value> parse_number()
    {

        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++m_position;
        }
        if (!(peek() >= '0' && peek() <= '9'))
            return fail("expected a digit");
        // Digits of the significand, the first 19 exactly.
        u64 significand = 0;
        int significant_digits = 0;
        int exponent_adjust = 0;
        bool integer_overflow = false;
        auto take_digit = [&](char c, bool fractional) {
            if (significant_digits < 19) {
                if (significand != 0 || c != '0')
                    ++significant_digits;
                significand = significand * 10 + static_cast<u64>(c - '0');
                if (fractional)
                    --exponent_adjust;
            } else {
                integer_overflow = true;
                if (!fractional)
                    ++exponent_adjust;
            }
        };
        if (peek() == '0') {
            ++m_position;
            if (peek() >= '0' && peek() <= '9')
                return fail("a number may not start with 0");
        } else {
            while (peek() >= '0' && peek() <= '9')
                take_digit(m_text[m_position++], false);
        }
        bool is_integer = true;
        if (peek() == '.') {
            is_integer = false;
            ++m_position;
            if (!(peek() >= '0' && peek() <= '9'))
                return fail("expected a digit after '.'");
            while (peek() >= '0' && peek() <= '9')
                take_digit(m_text[m_position++], true);
        }
        int exponent = 0;
        if (peek() == 'e' || peek() == 'E') {
            is_integer = false;
            ++m_position;
            bool negative_exponent = false;
            if (peek() == '+' || peek() == '-') {
                negative_exponent = peek() == '-';
                ++m_position;
            }
            if (!(peek() >= '0' && peek() <= '9'))
                return fail("expected a digit in the exponent");
            while (peek() >= '0' && peek() <= '9') {
                if (exponent < 100000)
                    exponent = exponent * 10 + (m_text[m_position] - '0');
                ++m_position;
            }
            if (negative_exponent)
                exponent = -exponent;
        }
        if (is_integer && !integer_overflow && (significand <= 0x7fffffffffffffffULL || (negative && significand == 0x8000000000000000ULL))) {
            i64 value = negative ? static_cast<i64>(0 - significand) : static_cast<i64>(significand);
            return Value(value);
        }
        // significand * 10^(exponent + adjust), by powers of ten.
        int power = exponent + exponent_adjust;
        double value = static_cast<double>(significand);
        double scale = 10;
        bool divide = power < 0;
        unsigned remaining = static_cast<unsigned>(divide ? -power : power);
        double factor = 1;
        while (remaining > 0) {
            if (remaining & 1)
                factor *= scale;
            scale *= scale;
            remaining >>= 1;
            if (factor > 1e308 || scale > 1e308)
                break;
        }
        value = divide ? value / factor : value * factor;
        return Value(negative ? -value : value);
    }

    StringView m_text;
    size_t m_position { 0 };
    ParseError* m_error;
};

// The shortest of 15, 16 or 17 significant digits that reads back as the
// same number, in plain or exponent notation as JavaScript would choose.
ErrorOr<void> append_number(StringBuilder& builder, double value)
{
    if (value != value || value - value != 0) {
        // NaN and the infinities have no JSON form.
        builder.append("null"_sv);
        return {};
    }
    if (value == 0) {
        builder.append('0');
        return {};
    }
    if (value < 0) {
        builder.append('-');
        value = -value;
    }
    // value = digits * 10^(decimal_exponent - precision + 1)
    int decimal_exponent = 0;
    double scaled = value;
    while (scaled >= 10) {
        scaled /= 10;
        ++decimal_exponent;
    }
    while (scaled < 1) {
        scaled *= 10;
        --decimal_exponent;
    }
    char digits[20];
    int count = 0;
    for (int precision = 15; precision <= 17; ++precision) {
        double remainder = scaled;
        u64 whole = 0;
        for (int i = 0; i < precision; ++i) {
            int digit = static_cast<int>(remainder);
            if (digit > 9)
                digit = 9;
            whole = whole * 10 + static_cast<u64>(digit);
            remainder = (remainder - digit) * 10;
        }
        if (remainder >= 5)
            ++whole;
        // Rounding may carry into a new digit.
        u64 limit = 1;
        for (int i = 0; i < precision; ++i)
            limit *= 10;
        int exponent_here = decimal_exponent;
        if (whole >= limit) {
            whole /= 10;
            ++exponent_here;
        }
        count = 0;
        for (u64 rest = whole; count < precision; rest /= 10)
            digits[precision - 1 - count++] = static_cast<char>('0' + rest % 10);
        while (count > 1 && digits[count - 1] == '0')
            --count;
        // Does it read back?
        StringBuilder trial;
        trial.append(StringView(digits, 1));
        if (count > 1) {
            trial.append('.');
            trial.append(StringView(digits + 1, static_cast<size_t>(count - 1)));
        }
        trial.appendff("e{}", exponent_here);
        auto back = parse(trial.view());
        decimal_exponent = exponent_here;
        if (!back.is_error() && back.value().is_number() && back.value().as_number() == value)
            break;
    }
    if (decimal_exponent >= -7 && decimal_exponent < 21) {
        if (decimal_exponent < 0) {
            builder.append("0."_sv);
            for (int i = -1; i > decimal_exponent; --i)
                builder.append('0');
            builder.append(StringView(digits, static_cast<size_t>(count)));
        } else {
            for (int i = 0; i <= decimal_exponent || i < count; ++i) {
                if (i == decimal_exponent + 1)
                    builder.append('.');
                builder.append(i < count ? digits[i] : '0');
            }
        }
        return {};
    }
    builder.append(digits[0]);
    if (count > 1) {
        builder.append('.');
        builder.append(StringView(digits + 1, static_cast<size_t>(count - 1)));
    }
    builder.appendff("e{}{}", decimal_exponent < 0 ? "-"_sv : "+"_sv, decimal_exponent < 0 ? -decimal_exponent : decimal_exponent);
    return {};
}

void append_quoted(StringBuilder& builder, StringView text)
{
    static constexpr char hex[] = "0123456789abcdef";
    builder.append('"');
    for (char c : text) {
        switch (c) {
        case '"':
            builder.append("\\\""_sv);
            break;
        case '\\':
            builder.append("\\\\"_sv);
            break;
        case '\n':
            builder.append("\\n"_sv);
            break;
        case '\r':
            builder.append("\\r"_sv);
            break;
        case '\t':
            builder.append("\\t"_sv);
            break;
        case '\b':
            builder.append("\\b"_sv);
            break;
        case '\f':
            builder.append("\\f"_sv);
            break;
        default:
            if (static_cast<u8>(c) < 0x20) {
                builder.append("\\u00"_sv);
                builder.append(hex[static_cast<u8>(c) >> 4]);
                builder.append(hex[static_cast<u8>(c) & 15]);
            } else {
                builder.append(c);
            }
        }
    }
    builder.append('"');
}

void indent(StringBuilder& builder, size_t depth)
{
    for (size_t i = 0; i < depth; ++i)
        builder.append("    "_sv);
}

}

ErrorOr<Value> Value::string(StringView text)
{
    Value value(Type::String);
    value.m_string = TRY(String::from_view(text));
    return value;
}

ErrorOr<void> Value::append(Value value)
{
    VERIFY(is_array());
    return m_elements.try_append(move(value));
}

Value const* Value::get(StringView key) const
{
    if (!is_object())
        return nullptr;
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i].view() == key)
            return &m_elements[i];
    }
    return nullptr;
}

ErrorOr<void> Value::set(StringView key, Value value)
{
    VERIFY(is_object());
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i].view() == key) {
            m_elements[i] = move(value);
            return {};
        }
    }
    TRY(m_keys.try_append(TRY(String::from_view(key))));
    if (auto result = m_elements.try_append(move(value)); result.is_error()) {
        m_keys.take_last();
        return result.release_error();
    }
    return {};
}

bool Value::remove(StringView key)
{
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i].view() == key) {
            m_keys.remove(i);
            m_elements.remove(i);
            return true;
        }
    }
    return false;
}

Optional<StringView> Value::get_string(StringView key) const
{
    auto const* value = get(key);
    if (!value || !value->is_string())
        return {};
    return value->as_string();
}

Optional<bool> Value::get_boolean(StringView key) const
{
    auto const* value = get(key);
    if (!value || !value->is_boolean())
        return {};
    return value->as_boolean();
}

Optional<i64> Value::get_integer(StringView key) const
{
    auto const* value = get(key);
    if (!value || !value->is_integer())
        return {};
    return value->as_integer();
}

Optional<double> Value::get_number(StringView key) const
{
    auto const* value = get(key);
    if (!value || !value->is_number())
        return {};
    return value->as_number();
}

Value const* Value::get_array(StringView key) const
{
    auto const* value = get(key);
    return value && value->is_array() ? value : nullptr;
}

Value const* Value::get_object(StringView key) const
{
    auto const* value = get(key);
    return value && value->is_object() ? value : nullptr;
}

bool Value::operator==(Value const& other) const
{
    if (m_type != other.m_type)
        return false;
    switch (m_type) {
    case Type::Null:
        return true;
    case Type::Boolean:
        return m_boolean == other.m_boolean;
    case Type::Number:
        if (m_is_integer && other.m_is_integer)
            return m_integer == other.m_integer;
        return m_number == other.m_number;
    case Type::String:
        return m_string.view() == other.m_string.view();
    case Type::Array:
    case Type::Object:
        if (m_elements.size() != other.m_elements.size())
            return false;
        for (size_t i = 0; i < m_elements.size(); ++i) {
            if (m_type == Type::Object && m_keys[i].view() != other.m_keys[i].view())
                return false;
            if (!(m_elements[i] == other.m_elements[i]))
                return false;
        }
        return true;
    }
    return false;
}

ErrorOr<void> Value::serialize_into(StringBuilder& builder, bool pretty, size_t depth) const
{
    switch (m_type) {
    case Type::Null:
        builder.append("null"_sv);
        return {};
    case Type::Boolean:
        builder.append(m_boolean ? "true"_sv : "false"_sv);
        return {};
    case Type::Number:
        if (m_is_integer) {
            builder.appendff("{}", m_integer);
            return {};
        }
        return append_number(builder, m_number);
    case Type::String:
        append_quoted(builder, m_string.view());
        return {};
    case Type::Array:
    case Type::Object: {
        bool object = m_type == Type::Object;
        builder.append(object ? '{' : '[');
        if (m_elements.is_empty()) {
            builder.append(object ? '}' : ']');
            return {};
        }
        for (size_t i = 0; i < m_elements.size(); ++i) {
            if (i > 0)
                builder.append(',');
            if (pretty) {
                builder.append('\n');
                indent(builder, depth + 1);
            }
            if (object) {
                append_quoted(builder, m_keys[i].view());
                builder.append(pretty ? " : "_sv : ":"_sv);
            }
            TRY(m_elements[i].serialize_into(builder, pretty, depth + 1));
        }
        if (pretty) {
            builder.append('\n');
            indent(builder, depth);
        }
        builder.append(object ? '}' : ']');
        return {};
    }
    }
    return {};
}

ErrorOr<String> Value::serialize(bool pretty) const
{
    StringBuilder builder;
    TRY(serialize_into(builder, pretty, 0));
    return builder.to_string();
}

ErrorOr<Value> parse(StringView text, ParseError* error)
{
    Parser parser(text, error);
    return parser.parse_document();
}

}
