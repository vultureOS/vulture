/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Format.h>

namespace Base {

static void write_repeated(FormatSink& sink, char character, size_t count)
{
    char chunk[32];
    for (char& slot : chunk)
        slot = character;
    while (count > 0) {
        size_t piece = min(count, sizeof(chunk));
        sink.write(StringView(chunk, piece));
        count -= piece;
    }
}

void write_padded(FormatSink& sink, FormatSpec const& spec, StringView content, FormatSpec::Align default_align)
{
    if (spec.width <= content.length()) {
        sink.write(content);
        return;
    }
    size_t padding = spec.width - content.length();
    auto align = spec.align == FormatSpec::Align::Default ? default_align : spec.align;
    size_t before = 0;
    if (align == FormatSpec::Align::Right)
        before = padding;
    else if (align == FormatSpec::Align::Center)
        before = padding / 2;
    write_repeated(sink, spec.fill, before);
    sink.write(content);
    write_repeated(sink, spec.fill, padding - before);
}

void format_integer(FormatSink& sink, FormatSpec const& spec, u64 magnitude, bool is_negative)
{
    if (spec.type == 'c') {
        VERIFY(!is_negative && magnitude < 0x80);
        char character = static_cast<char>(magnitude);
        write_padded(sink, spec, StringView(&character, 1), FormatSpec::Align::Left);
        return;
    }

    u32 base = 10;
    bool uppercase = false;
    StringView prefix;
    switch (spec.type) {
    case 0:
    case 'd':
        break;
    case 'x':
        base = 16;
        prefix = "0x"_sv;
        break;
    case 'X':
        base = 16;
        uppercase = true;
        prefix = "0X"_sv;
        break;
    case 'b':
        base = 2;
        prefix = "0b"_sv;
        break;
    case 'o':
        base = 8;
        prefix = "0o"_sv;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    // Digits come out least significant first.
    char digits[64];
    size_t digit_count = 0;
    do {
        u32 digit = static_cast<u32>(magnitude % base);
        digits[digit_count++] = static_cast<char>(digit < 10 ? '0' + digit : (uppercase ? 'A' : 'a') + digit - 10);
        magnitude /= base;
    } while (magnitude != 0);

    StringView sign;
    if (is_negative)
        sign = "-"_sv;
    else if (spec.sign == FormatSpec::Sign::Always)
        sign = "+"_sv;
    if (!spec.alternate_form)
        prefix = {};

    char buffer[1 + 2 + 64];
    size_t length = 0;
    for (char character : sign)
        buffer[length++] = character;
    for (char character : prefix)
        buffer[length++] = character;

    if (spec.zero_pad && spec.align == FormatSpec::Align::Default) {
        sink.write(StringView(buffer, length));
        size_t used = length + digit_count;
        if (spec.width > used)
            write_repeated(sink, '0', spec.width - used);
        length = 0;
    }

    while (digit_count > 0)
        buffer[length++] = digits[--digit_count];

    if (spec.zero_pad && spec.align == FormatSpec::Align::Default)
        sink.write(StringView(buffer, length));
    else
        write_padded(sink, spec, StringView(buffer, length), FormatSpec::Align::Right);
}

void format_pointer(FormatSink& sink, FormatSpec const& spec, FlatPtr address)
{
    FormatSpec pointer_spec = spec;
    pointer_spec.type = 'x';
    pointer_spec.alternate_form = true;
    if (spec.width == 0) {
        pointer_spec.zero_pad = true;
        pointer_spec.width = 2 + 2 * sizeof(FlatPtr);
    }
    format_integer(sink, pointer_spec, address, false);
}

void Formatter<bool>::format(FormatSink& sink, FormatSpec const& spec, bool value)
{
    write_padded(sink, spec, value ? "true"_sv : "false"_sv, FormatSpec::Align::Left);
}

void Formatter<char>::format(FormatSink& sink, FormatSpec const& spec, char value)
{
    if (spec.type != 0 && spec.type != 'c') {
        Formatter<u8>::format(sink, spec, static_cast<u8>(value));
        return;
    }
    write_padded(sink, spec, StringView(&value, 1), FormatSpec::Align::Left);
}

void Formatter<StringView>::format(FormatSink& sink, FormatSpec const& spec, StringView value)
{
    write_padded(sink, spec, value, FormatSpec::Align::Left);
}

void Formatter<char const*>::format(FormatSink& sink, FormatSpec const& spec, char const* value)
{
    write_padded(sink, spec, value ? StringView(value) : "(null)"_sv, FormatSpec::Align::Left);
}

void Formatter<nullptr_t>::format(FormatSink& sink, FormatSpec const& spec, nullptr_t)
{
    write_padded(sink, spec, "nullptr"_sv, FormatSpec::Align::Left);
}

static constexpr bool is_align_character(char character)
{
    return character == '<' || character == '>' || character == '^';
}

static constexpr FormatSpec::Align align_from_character(char character)
{
    switch (character) {
    case '<':
        return FormatSpec::Align::Left;
    case '>':
        return FormatSpec::Align::Right;
    default:
        return FormatSpec::Align::Center;
    }
}

static FormatSpec parse_spec(StringView text)
{
    FormatSpec spec;
    size_t index = 0;
    if (text.length() >= 2 && is_align_character(text[1])) {
        spec.fill = text[0];
        spec.align = align_from_character(text[1]);
        index = 2;
    } else if (!text.is_empty() && is_align_character(text[0])) {
        spec.align = align_from_character(text[0]);
        index = 1;
    }
    if (index < text.length() && text[index] == '+') {
        spec.sign = FormatSpec::Sign::Always;
        ++index;
    }
    if (index < text.length() && text[index] == '#') {
        spec.alternate_form = true;
        ++index;
    }
    if (index < text.length() && text[index] == '0') {
        spec.zero_pad = true;
        ++index;
    }
    while (index < text.length() && is_ascii_digit(static_cast<u8>(text[index]))) {
        spec.width = spec.width * 10 + static_cast<size_t>(text[index] - '0');
        ++index;
    }
    if (index < text.length())
        spec.type = text[index++];
    VERIFY(index == text.length());
    return spec;
}

void vformat(FormatSink& sink, StringView format, Span<FormatArgument const> arguments)
{
    size_t next_argument = 0;
    size_t literal_start = 0;
    auto flush_literal = [&](size_t end) {
        if (end > literal_start)
            sink.write(format.substring_view(literal_start, end - literal_start));
    };

    for (size_t i = 0; i < format.length(); ++i) {
        char character = format[i];
        if (character == '{') {
            flush_literal(i);
            if (i + 1 < format.length() && format[i + 1] == '{') {
                sink.write("{"_sv);
                literal_start = ++i + 1;
                continue;
            }
            auto close = format.find('}', i + 1);
            VERIFY(close.has_value());
            auto inner = format.substring_view(i + 1, close.value() - i - 1);
            FormatSpec spec;
            if (!inner.is_empty()) {
                VERIFY(inner[0] == ':');
                spec = parse_spec(inner.substring_view(1));
            }
            VERIFY(next_argument < arguments.size());
            auto const& argument = arguments[next_argument++];
            argument.format(sink, spec, argument.value);
            i = close.value();
            literal_start = i + 1;
        } else if (character == '}') {
            flush_literal(i);
            VERIFY(i + 1 < format.length() && format[i + 1] == '}');
            sink.write("}"_sv);
            literal_start = ++i + 1;
        }
    }
    flush_literal(format.length());
}

}
