/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibJSON/PropertyList.h>

namespace JSON {

namespace {

class XMLReader {
public:
    explicit XMLReader(StringView text)
        : m_text(text)
    {
    }

    ErrorOr<Value> read_document()
    {
        skip_prolog();
        TRY(expect_open("plist"_sv));
        auto value = TRY(read_value());
        skip_blank();
        TRY(expect_close("plist"_sv));
        return value;
    }

private:
    void skip_blank()
    {
        while (m_position < m_text.length()) {
            char const c = m_text[m_position];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++m_position;
        }
    }

    void skip_prolog()
    {
        for (;;) {
            skip_blank();
            if (m_text.substring_view(m_position).starts_with("<?"_sv)) {
                auto const end = m_text.find("?>"_sv, m_position);
                m_position = end.has_value() ? end.value() + 2 : m_text.length();
            } else if (m_text.substring_view(m_position).starts_with("<!--"_sv)) {
                auto const end = m_text.find("-->"_sv, m_position);
                m_position = end.has_value() ? end.value() + 3 : m_text.length();
            } else if (m_text.substring_view(m_position).starts_with("<!"_sv)) {
                auto const end = m_text.find('>', m_position);
                m_position = end.has_value() ? end.value() + 1 : m_text.length();
            } else {
                return;
            }
        }
    }

    // The next tag's name, moving past it: "dict" for <dict>, "/dict" for </dict>, "dict/" for <dict/>.
    ErrorOr<String> read_tag()
    {
        skip_prolog();
        if (m_position >= m_text.length() || m_text[m_position] != '<')
            return Error::from_string_literal("a property list is missing a tag");
        auto const end = m_text.find('>', m_position);
        if (!end.has_value())
            return Error::from_string_literal("a property list has a tag that isn't finished");
        auto tag = m_text.substring_view(m_position + 1, end.value() - m_position - 1);
        m_position = end.value() + 1;
        // Attributes are of no interest, but a closing slash is.
        bool const self_closing = tag.ends_with('/');
        auto const space = tag.find(' ');
        if (space.has_value())
            tag = tag.substring_view(0, space.value());
        StringBuilder name;
        name.append(tag);
        if (self_closing && !tag.ends_with('/'))
            name.append('/');
        return name.to_string();
    }

    ErrorOr<void> expect_open(StringView wanted)
    {
        auto tag = TRY(read_tag());
        if (tag.view() != wanted)
            return Error::from_string_literal("this isn't a property list");
        return {};
    }

    ErrorOr<void> expect_close(StringView wanted)
    {
        auto tag = TRY(read_tag());
        if (!tag.view().starts_with('/') || tag.view().substring_view(1) != wanted)
            return Error::from_string_literal("a property list has mismatched tags");
        return {};
    }

    // The text up to the next tag, with its entities turned back.
    ErrorOr<String> read_text()
    {
        auto const end = m_text.find('<', m_position);
        if (!end.has_value())
            return Error::from_string_literal("a property list's text isn't finished");
        auto const raw = m_text.substring_view(m_position, end.value() - m_position);
        m_position = end.value();
        StringBuilder text;
        for (size_t i = 0; i < raw.length(); ++i) {
            if (raw[i] != '&') {
                text.append(raw[i]);
                continue;
            }
            auto const rest = raw.substring_view(i);
            struct Entity {
                StringView name;
                char value;
            };
            Entity const entities[] = { { "&amp;"_sv, '&' }, { "&lt;"_sv, '<' }, { "&gt;"_sv, '>' }, { "&quot;"_sv, '"' }, { "&apos;"_sv, '\'' } };
            bool found = false;
            for (auto const& entity : entities) {
                if (rest.starts_with(entity.name)) {
                    text.append(entity.value);
                    i += entity.name.length() - 1;
                    found = true;
                    break;
                }
            }
            if (!found)
                text.append('&');
        }
        return text.to_string();
    }

    ErrorOr<Value> read_value()
    {
        auto tag = TRY(read_tag());
        return read_value_of(tag.view());
    }

    ErrorOr<Value> read_value_of(StringView tag)
    {
        if (tag == "dict/"_sv)
            return Value::object();
        if (tag == "array/"_sv)
            return Value::array();
        if (tag == "true/"_sv)
            return Value(true);
        if (tag == "false/"_sv)
            return Value(false);
        if (tag == "string/"_sv)
            return Value::string({});
        if (tag == "dict"_sv) {
            auto object = Value::object();
            for (;;) {
                auto next = TRY(read_tag());
                if (next.view() == "/dict"_sv)
                    return object;
                if (next.view() != "key"_sv)
                    return Error::from_string_literal("a dictionary has something other than a key where a key should be");
                auto const key = TRY(read_text());
                TRY(expect_close("key"_sv));
                auto value = TRY(read_value());
                TRY(object.set(key.view(), move(value)));
            }
        }
        if (tag == "array"_sv) {
            auto array = Value::array();
            for (;;) {
                auto next = TRY(read_tag());
                if (next.view() == "/array"_sv)
                    return array;
                TRY(array.append(TRY(read_value_of(next.view()))));
            }
        }
        if (tag == "string"_sv || tag == "date"_sv || tag == "data"_sv) {
            auto const text = TRY(read_text());
            TRY(expect_close(tag));
            return Value::string(text.view());
        }
        if (tag == "integer"_sv) {
            auto const text = TRY(read_text());
            TRY(expect_close(tag));
            auto const number = text.view().trim_whitespace().to_number<i64>();
            if (!number.has_value())
                return Error::from_string_literal("a property list's integer isn't one");
            return Value(number.value());
        }
        if (tag == "real"_sv) {
            auto const text = TRY(read_text());
            TRY(expect_close(tag));
            // With what JSON already knows about reading numbers.
            auto parsed = parse(text.view().trim_whitespace());
            if (parsed.is_error() || !parsed.value().is_number())
                return Error::from_string_literal("a property list's real number isn't one");
            return parsed.release_value();
        }
        return Error::from_string_literal("a property list has a tag this doesn't know");
    }

    StringView m_text;
    size_t m_position { 0 };
};

// bplist00: an object table, addressed by an offset table, described by the trailer. See Apple's CFBinaryPList.c for the
// format's definition; this is written from that description.
class BinaryReader {
public:
    explicit BinaryReader(ReadonlyBytes data)
        : m_data(data)
    {
    }

    ErrorOr<Value> read_document()
    {
        if (m_data.size() < 8 + 32)
            return Error::from_string_literal("too short to be a binary property list");
        auto const trailer = m_data.slice(m_data.size() - 32);
        m_offset_size = trailer[6];
        m_reference_size = trailer[7];
        m_count = read_integer(trailer.slice(8, 8));
        u64 const top = read_integer(trailer.slice(16, 8));
        m_table = read_integer(trailer.slice(24, 8));
        if (m_offset_size == 0 || m_offset_size > 8 || m_reference_size == 0 || m_reference_size > 8 || m_table + m_count * m_offset_size > m_data.size() || top >= m_count)
            return Error::from_string_literal("a binary property list's trailer is damaged");
        return read_object(top, 0);
    }

private:
    static u64 read_integer(ReadonlyBytes bytes)
    {
        u64 value = 0;
        for (size_t i = 0; i < bytes.size(); ++i)
            value = (value << 8) | bytes[i];
        return value;
    }

    ErrorOr<size_t> object_offset(u64 index) const
    {
        if (index >= m_count)
            return Error::from_string_literal("a binary property list refers to what isn't there");
        size_t const at = m_table + index * m_offset_size;
        u64 const offset = read_integer(m_data.slice(at, m_offset_size));
        if (offset >= m_data.size())
            return Error::from_string_literal("a binary property list refers to what isn't there");
        return offset;
    }

    // How many follow, for what can have many.
    ErrorOr<u64> read_count(size_t& at, u8 low)
    {
        if (low != 0xf)
            return low;
        if (at >= m_data.size())
            return Error::from_string_literal("a binary property list is cut short");
        u8 const marker = m_data[at++];
        if ((marker & 0xf0) != 0x10)
            return Error::from_string_literal("a binary property list's count isn't one");
        size_t const size = 1ull << (marker & 0xf);
        if (at + size > m_data.size())
            return Error::from_string_literal("a binary property list is cut short");
        u64 const count = read_integer(m_data.slice(at, size));
        at += size;
        return count;
    }

    ErrorOr<Value> read_object(u64 index, int depth)
    {
        if (depth > 64)
            return Error::from_string_literal("a binary property list goes too deep");
        size_t at = TRY(object_offset(index));
        u8 const marker = m_data[at++];
        u8 const high = marker >> 4;
        u8 const low = marker & 0xf;
        switch (high) {
        case 0x0:
            if (low == 0x8)
                return Value(false);
            if (low == 0x9)
                return Value(true);
            return Value {};
        case 0x1: {
            size_t const size = 1ull << low;
            if (at + size > m_data.size())
                return Error::from_string_literal("a binary property list is cut short");
            return Value(static_cast<i64>(read_integer(m_data.slice(at, size))));
        }
        case 0x2:
        case 0x3: {
            size_t const size = high == 0x3 ? 8 : 1ull << low;
            if (at + size > m_data.size() || (size != 4 && size != 8))
                return Error::from_string_literal("a binary property list's number is damaged");
            u64 const bits = read_integer(m_data.slice(at, size));
            if (size == 4)
                return Value(static_cast<double>(__builtin_bit_cast(float, static_cast<u32>(bits))));
            return Value(__builtin_bit_cast(double, bits));
        }
        case 0x5: {
            u64 const count = TRY(read_count(at, low));
            if (at + count > m_data.size())
                return Error::from_string_literal("a binary property list is cut short");
            return Value::string(StringView(m_data.slice(at, count)));
        }
        case 0x6: {
            u64 const count = TRY(read_count(at, low));
            if (at + count * 2 > m_data.size())
                return Error::from_string_literal("a binary property list is cut short");
            // UTF-16, big-endian.
            StringBuilder text;
            for (u64 i = 0; i < count; ++i) {
                u32 code = static_cast<u32>(m_data[at + i * 2]) << 8 | m_data[at + i * 2 + 1];
                if (code >= 0xd800 && code < 0xdc00 && i + 1 < count) {
                    u32 const low_surrogate = static_cast<u32>(m_data[at + (i + 1) * 2]) << 8 | m_data[at + (i + 1) * 2 + 1];
                    code = 0x10000 + ((code - 0xd800) << 10) + (low_surrogate - 0xdc00);
                    ++i;
                }
                if (code < 0x80) {
                    text.append(static_cast<char>(code));
                } else if (code < 0x800) {
                    text.append(static_cast<char>(0xc0 | (code >> 6)));
                    text.append(static_cast<char>(0x80 | (code & 0x3f)));
                } else if (code < 0x10000) {
                    text.append(static_cast<char>(0xe0 | (code >> 12)));
                    text.append(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    text.append(static_cast<char>(0x80 | (code & 0x3f)));
                } else {
                    text.append(static_cast<char>(0xf0 | (code >> 18)));
                    text.append(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                    text.append(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    text.append(static_cast<char>(0x80 | (code & 0x3f)));
                }
            }
            return Value::string(text.view());
        }
        case 0x4: {
            // Data, which is shown as how many bytes it is.
            u64 const count = TRY(read_count(at, low));
            char description[64];
            __builtin_snprintf(description, sizeof(description), "<%llu bytes>", static_cast<unsigned long long>(count));
            return Value::string(StringView(description));
        }
        case 0xa: {
            u64 const count = TRY(read_count(at, low));
            if (at + count * m_reference_size > m_data.size())
                return Error::from_string_literal("a binary property list is cut short");
            auto array = Value::array();
            for (u64 i = 0; i < count; ++i)
                TRY(array.append(TRY(read_object(read_integer(m_data.slice(at + i * m_reference_size, m_reference_size)), depth + 1))));
            return array;
        }
        case 0xd: {
            u64 const count = TRY(read_count(at, low));
            if (at + count * 2 * m_reference_size > m_data.size())
                return Error::from_string_literal("a binary property list is cut short");
            auto object = Value::object();
            for (u64 i = 0; i < count; ++i) {
                auto key = TRY(read_object(read_integer(m_data.slice(at + i * m_reference_size, m_reference_size)), depth + 1));
                if (!key.is_string())
                    return Error::from_string_literal("a binary property list's key isn't a string");
                auto value = TRY(read_object(read_integer(m_data.slice(at + (count + i) * m_reference_size, m_reference_size)), depth + 1));
                TRY(object.set(key.as_string(), move(value)));
            }
            return object;
        }
        default:
            return Error::from_string_literal("a binary property list has what this doesn't know");
        }
    }

    ReadonlyBytes m_data;
    size_t m_offset_size { 0 };
    size_t m_reference_size { 0 };
    u64 m_count { 0 };
    u64 m_table { 0 };
};

void escape_xml(StringView text, StringBuilder& out)
{
    for (char const c : text) {
        switch (c) {
        case '&':
            out.append("&amp;"_sv);
            break;
        case '<':
            out.append("&lt;"_sv);
            break;
        case '>':
            out.append("&gt;"_sv);
            break;
        default:
            out.append(c);
            break;
        }
    }
}

void indent(StringBuilder& out, size_t depth, char with)
{
    for (size_t i = 0; i < depth; ++i)
        out.append(with);
}

void write_xml(Value const& value, StringBuilder& out, size_t depth)
{
    indent(out, depth, '\t');
    if (value.is_object()) {
        if (value.size() == 0) {
            out.append("<dict/>\n"_sv);
            return;
        }
        out.append("<dict>\n"_sv);
        for (size_t i = 0; i < value.size(); ++i) {
            indent(out, depth + 1, '\t');
            out.append("<key>"_sv);
            escape_xml(value.key_at(i), out);
            out.append("</key>\n"_sv);
            write_xml(value.at(i), out, depth + 1);
        }
        indent(out, depth, '\t');
        out.append("</dict>\n"_sv);
    } else if (value.is_array()) {
        if (value.size() == 0) {
            out.append("<array/>\n"_sv);
            return;
        }
        out.append("<array>\n"_sv);
        for (size_t i = 0; i < value.size(); ++i)
            write_xml(value.at(i), out, depth + 1);
        indent(out, depth, '\t');
        out.append("</array>\n"_sv);
    } else if (value.is_string()) {
        out.append("<string>"_sv);
        escape_xml(value.as_string(), out);
        out.append("</string>\n"_sv);
    } else if (value.is_boolean()) {
        out.append(value.as_boolean() ? "<true/>\n"_sv : "<false/>\n"_sv);
    } else if (value.is_number()) {
        double const number = value.as_number();
        char buffer[64];
        if (number == static_cast<double>(static_cast<i64>(number))) {
            __builtin_snprintf(buffer, sizeof(buffer), "<integer>%lld</integer>\n", static_cast<long long>(number));
        } else {
            __builtin_snprintf(buffer, sizeof(buffer), "<real>%.17g</real>\n", number);
        }
        out.append(StringView(buffer));
    } else {
        out.append("<string></string>\n"_sv);
    }
}

// Quotes it, unless it's all things that needn't be, as `defaults read` does.
void describe_string(StringView text, StringBuilder& out)
{
    bool plain = !text.is_empty();
    for (char const c : text) {
        bool const safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' || c == '$';
        plain = plain && safe;
    }
    if (plain) {
        out.append(text);
        return;
    }
    out.append('"');
    for (char const c : text) {
        if (c == '"' || c == '\\')
            out.append('\\');
        if (c == '\n') {
            out.append("\\n"_sv);
            continue;
        }
        out.append(c);
    }
    out.append('"');
}

Vector<size_t> sorted_keys(Value const& dictionary)
{
    Vector<size_t> order;
    for (size_t i = 0; i < dictionary.size(); ++i) {
        MUST(order.try_append(i));
        size_t j = i;
        while (j > 0) {
            auto const a = dictionary.key_at(order[j - 1]);
            auto const b = dictionary.key_at(order[j]);
            size_t const shared = min(a.length(), b.length());
            int const compared = shared ? __builtin_memcmp(a.characters_without_null_termination(), b.characters_without_null_termination(), shared) : 0;
            if (compared < 0 || (compared == 0 && a.length() <= b.length()))
                break;
            swap(order[j - 1], order[j]);
            --j;
        }
    }
    return order;
}

// plutil -p's, which isn't what `defaults read` says.
void print_for_people(Value const& value, StringBuilder& out, size_t depth)
{
    if (value.is_object()) {
        out.append("{\n"_sv);
        for (size_t place : sorted_keys(value)) {
            indent(out, (depth + 1) * 2, ' ');
            out.append('"');
            out.append(value.key_at(place));
            out.append("\" => "_sv);
            print_for_people(value.at(place), out, depth + 1);
            out.append('\n');
        }
        indent(out, depth * 2, ' ');
        out.append('}');
    } else if (value.is_array()) {
        out.append("[\n"_sv);
        for (size_t i = 0; i < value.size(); ++i) {
            indent(out, (depth + 1) * 2, ' ');
            char number[32];
            __builtin_snprintf(number, sizeof(number), "%zu", i);
            out.append(StringView(number));
            out.append(" => "_sv);
            print_for_people(value.at(i), out, depth + 1);
            out.append('\n');
        }
        indent(out, depth * 2, ' ');
        out.append(']');
    } else if (value.is_string()) {
        out.append('"');
        out.append(value.as_string());
        out.append('"');
    } else if (value.is_boolean()) {
        out.append(value.as_boolean() ? "true"_sv : "false"_sv);
    } else if (value.is_number()) {
        char buffer[64];
        if (value.is_integer())
            __builtin_snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value.as_integer()));
        else
            __builtin_snprintf(buffer, sizeof(buffer), "%.17g", value.as_number());
        out.append(StringView(buffer));
    }
}

// The binary form: every value in a table, and what's in a collection is its place in that table.
class BinaryWriter {
public:
    ErrorOr<Vector<u8>> write(Value const& top)
    {
        count_values(top);
        m_reference_size = m_value_count < 256 ? 1 : (m_value_count < 65536 ? 2 : 4);
        put_text("bplist00"_sv);
        u64 const top_place = TRY(add(top));
        // Where each one is, then what that's all been.
        u64 const table_at = m_bytes.size();
        u64 const largest = table_at;
        u8 const offset_size = largest < 256 ? 1 : (largest < 65536 ? 2 : (largest < (1ull << 32) ? 4 : 8));
        for (u64 offset : m_offsets)
            put_big_endian(offset, offset_size);
        for (int i = 0; i < 6; ++i)
            TRY(m_bytes.try_append(0));
        TRY(m_bytes.try_append(offset_size));
        TRY(m_bytes.try_append(m_reference_size));
        put_big_endian(m_offsets.size(), 8);
        put_big_endian(top_place, 8);
        put_big_endian(table_at, 8);
        return move(m_bytes);
    }

private:
    void count_values(Value const& value)
    {
        ++m_value_count;
        if (value.is_object()) {
            m_value_count += value.size();
            for (size_t i = 0; i < value.size(); ++i)
                count_values(value.at(i));
        } else if (value.is_array()) {
            for (size_t i = 0; i < value.size(); ++i)
                count_values(value.at(i));
        }
    }

    void put_text(StringView text)
    {
        for (size_t i = 0; i < text.length(); ++i)
            MUST(m_bytes.try_append(static_cast<u8>(text[i])));
    }

    void put_big_endian(u64 value, size_t size)
    {
        for (size_t i = size; i > 0; --i)
            MUST(m_bytes.try_append(static_cast<u8>(value >> ((i - 1) * 8))));
    }

    // A marker, with a length in it if that'll fit; otherwise as an integer after it.
    void put_marker(u8 kind, size_t length)
    {
        if (length < 15) {
            MUST(m_bytes.try_append(static_cast<u8>(kind | length)));
            return;
        }
        MUST(m_bytes.try_append(static_cast<u8>(kind | 15)));
        put_integer(static_cast<i64>(length));
    }

    void put_integer(i64 value)
    {
        if (value >= 0 && value < 256) {
            MUST(m_bytes.try_append(0x10));
            put_big_endian(static_cast<u64>(value), 1);
        } else if (value >= 0 && value < 65536) {
            MUST(m_bytes.try_append(0x11));
            put_big_endian(static_cast<u64>(value), 2);
        } else if (value >= 0 && value < (1ll << 31)) {
            MUST(m_bytes.try_append(0x12));
            put_big_endian(static_cast<u64>(value), 4);
        } else {
            MUST(m_bytes.try_append(0x13));
            put_big_endian(static_cast<u64>(value), 8);
        }
    }

    ErrorOr<u64> add(Value const& value)
    {
        u64 const place = m_offsets.size();
        TRY(m_offsets.try_append(0));
        // What's in a collection goes after it, so its references are worked out first.
        if (value.is_object() || value.is_array()) {
            bool const is_dictionary = value.is_object();
            Vector<size_t> order;
            if (is_dictionary)
                order = sorted_keys(value);
            size_t const count = value.size();
            Vector<u64> references;
            if (is_dictionary) {
                for (size_t i = 0; i < count; ++i) {
                    auto const key = TRY(Value::string(value.key_at(order[i])));
                    TRY(references.try_append(TRY(add(key))));
                }
            }
            for (size_t i = 0; i < count; ++i)
                TRY(references.try_append(TRY(add(value.at(is_dictionary ? order[i] : i)))));
            m_offsets[place] = m_bytes.size();
            put_marker(is_dictionary ? 0xd0 : 0xa0, count);
            for (u64 reference : references)
                put_big_endian(reference, m_reference_size);
            return place;
        }
        m_offsets[place] = m_bytes.size();
        if (value.is_null()) {
            TRY(m_bytes.try_append(0x00));
        } else if (value.is_boolean()) {
            TRY(m_bytes.try_append(value.as_boolean() ? 0x09 : 0x08));
        } else if (value.is_integer()) {
            put_integer(value.as_integer());
        } else if (value.is_number()) {
            TRY(m_bytes.try_append(0x23));
            double const number = value.as_number();
            u64 bits;
            __builtin_memcpy(&bits, &number, sizeof(bits));
            put_big_endian(bits, 8);
        } else {
            auto const text = value.as_string();
            bool ascii = true;
            for (size_t i = 0; i < text.length(); ++i)
                ascii = ascii && static_cast<u8>(text[i]) < 0x80;
            if (ascii) {
                put_marker(0x50, text.length());
                put_text(text);
            } else {
                // As UTF-16, a unit at a time.
                Vector<u16> units;
                for (size_t i = 0; i < text.length();) {
                    u8 const lead = static_cast<u8>(text[i]);
                    u32 scalar = lead;
                    size_t extra = lead >= 0xf0 ? 3 : (lead >= 0xe0 ? 2 : (lead >= 0xc0 ? 1 : 0));
                    if (extra)
                        scalar = lead & (0xff >> (extra + 2));
                    for (size_t n = 1; n <= extra && i + n < text.length(); ++n)
                        scalar = (scalar << 6) | (static_cast<u8>(text[i + n]) & 0x3f);
                    i += extra + 1;
                    if (scalar >= 0x10000) {
                        scalar -= 0x10000;
                        TRY(units.try_append(static_cast<u16>(0xd800 | (scalar >> 10))));
                        TRY(units.try_append(static_cast<u16>(0xdc00 | (scalar & 0x3ff))));
                    } else {
                        TRY(units.try_append(static_cast<u16>(scalar)));
                    }
                }
                put_marker(0x60, units.size());
                for (u16 unit : units)
                    put_big_endian(unit, 2);
            }
        }
        return place;
    }

    Vector<u8> m_bytes;
    Vector<u64> m_offsets;
    size_t m_value_count { 0 };
    u8 m_reference_size { 1 };
};

void describe_value(Value const& value, StringBuilder& out, size_t depth)
{
    if (value.is_object()) {
        out.append("{\n"_sv);
        // In order, as a dictionary has none of its own.
        for (size_t place : sorted_keys(value)) {
            indent(out, (depth + 1) * 4, ' ');
            describe_string(value.key_at(place), out);
            out.append(" = "_sv);
            describe_value(value.at(place), out, depth + 1);
            out.append(";\n"_sv);
        }
        indent(out, depth * 4, ' ');
        out.append('}');
    } else if (value.is_array()) {
        out.append("(\n"_sv);
        for (size_t i = 0; i < value.size(); ++i) {
            indent(out, (depth + 1) * 4, ' ');
            describe_value(value.at(i), out, depth + 1);
            if (i + 1 < value.size())
                out.append(',');
            out.append('\n');
        }
        indent(out, depth * 4, ' ');
        out.append(')');
    } else if (value.is_string()) {
        describe_string(value.as_string(), out);
    } else if (value.is_boolean()) {
        out.append(value.as_boolean() ? '1' : '0');
    } else if (value.is_number()) {
        double const number = value.as_number();
        char buffer[64];
        if (number == static_cast<double>(static_cast<i64>(number)))
            __builtin_snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(number));
        else
            __builtin_snprintf(buffer, sizeof(buffer), "%.17g", number);
        out.append(StringView(buffer));
    }
}

}

ErrorOr<String> print_property_list_for_people(Value const& value)
{
    StringBuilder out;
    print_for_people(value, out, 0);
    return out.to_string();
}

ErrorOr<Vector<u8>> serialize_property_list_binary(Value const& value)
{
    BinaryWriter writer;
    return writer.write(value);
}

ErrorOr<Value> parse_property_list(ReadonlyBytes data)
{
    if (data.size() >= 8 && StringView(data.slice(0, 8)) == "bplist00"_sv)
        return BinaryReader(data).read_document();
    auto const text = StringView(data).trim_whitespace();
    if (text.starts_with('<'))
        return XMLReader(text).read_document();
    return parse(text);
}

ErrorOr<String> serialize_property_list_xml(Value const& value)
{
    StringBuilder out;
    out.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n"_sv);
    write_xml(value, out, 0);
    out.append("</plist>\n"_sv);
    return out.to_string();
}

ErrorOr<String> describe_property_list(Value const& value)
{
    StringBuilder out;
    describe_value(value, out, 0);
    return out.to_string();
}

}
