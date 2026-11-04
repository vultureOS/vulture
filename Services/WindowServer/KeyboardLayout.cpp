/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyboardLayout.h"
#include <fcntl.h>
#include <unistd.h>

namespace {

struct Plain {
    u8 code;
    char plain;
    char shifted;
};

// Enough to type with if there's no file: where nothing else is known, keys are what they are on a US keyboard.
constexpr Plain s_builtin[] = {
    { 2, '1', '!' }, { 3, '2', '@' }, { 4, '3', '#' }, { 5, '4', '$' }, { 6, '5', '%' }, { 7, '6', '^' }, { 8, '7', '&' }, { 9, '8', '*' }, { 10, '9', '(' }, { 11, '0', ')' }, { 12, '-', '_' }, { 13, '=', '+' },
    { 16, 'q', 'Q' }, { 17, 'w', 'W' }, { 18, 'e', 'E' }, { 19, 'r', 'R' }, { 20, 't', 'T' }, { 21, 'y', 'Y' }, { 22, 'u', 'U' }, { 23, 'i', 'I' }, { 24, 'o', 'O' }, { 25, 'p', 'P' }, { 26, '[', '{' }, { 27, ']', '}' },
    { 30, 'a', 'A' }, { 31, 's', 'S' }, { 32, 'd', 'D' }, { 33, 'f', 'F' }, { 34, 'g', 'G' }, { 35, 'h', 'H' }, { 36, 'j', 'J' }, { 37, 'k', 'K' }, { 38, 'l', 'L' }, { 39, ';', ':' }, { 40, '\'', '"' }, { 41, '`', '~' },
    { 43, '\\', '|' }, { 44, 'z', 'Z' }, { 45, 'x', 'X' }, { 46, 'c', 'C' }, { 47, 'v', 'V' }, { 48, 'b', 'B' }, { 49, 'n', 'N' }, { 50, 'm', 'M' }, { 51, ',', '<' }, { 52, '.', '>' }, { 53, '/', '?' }, { 57, ' ', ' ' },
};

// Keys that do the same on every keyboard.
char always_the_same(u16 code)
{
    switch (code) {
    case 14:
        return 8;
    case 15:
        return '\t';
    case 28:
        return '\r';
    case 55:
        return '*';
    default:
        return 0;
    }
}

ErrorOr<String> utf8_from_hex_utf16(StringView hex)
{
    StringBuilder out;
    u32 pending_high = 0;
    for (size_t at = 0; at + 4 <= hex.length(); at += 4) {
        u32 unit = 0;
        for (size_t i = 0; i < 4; ++i) {
            char const c = hex[at + i];
            unit = unit * 16 + static_cast<u32>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        u32 code = unit;
        if (unit >= 0xd800 && unit < 0xdc00) {
            pending_high = unit;
            continue;
        }
        if (unit >= 0xdc00 && unit < 0xe000 && pending_high) {
            code = 0x10000 + ((pending_high - 0xd800) << 10) + (unit - 0xdc00);
            pending_high = 0;
        }
        out.append_code_point(code);
    }
    return out.to_string();
}

ErrorOr<Vector<char>> read_file(StringView path)
{
    StringBuilder terminated;
    terminated.append(path);
    auto const full_path = TRY(terminated.to_string());
    int const fd = open(full_path.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_string_literal("no such keyboard layout");
    Vector<char> bytes;
    char chunk[4096];
    for (;;) {
        ssize_t const got = read(fd, chunk, sizeof(chunk));
        if (got <= 0)
            break;
        size_t const at = bytes.size();
        TRY(bytes.try_resize(at + static_cast<size_t>(got)));
        __builtin_memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return bytes;
}

// The fields on a line.
Vector<StringView> fields_of(StringView line)
{
    Vector<StringView> fields;
    size_t at = 0;
    while (at < line.length()) {
        while (at < line.length() && line[at] == ' ')
            ++at;
        size_t const start = at;
        while (at < line.length() && line[at] != ' ')
            ++at;
        if (at > start)
            fields.append(line.substring_view(start, at - start));
    }
    return fields;
}

u32 number_of(StringView text)
{
    u32 value = 0;
    for (size_t i = 0; i < text.length(); ++i)
        value = value * 10 + static_cast<u32>(text[i] - '0');
    return value;
}

u8 layer_named(StringView name)
{
    if (name == "shift"_sv)
        return KeyboardLayout::Shift;
    if (name == "option"_sv)
        return KeyboardLayout::Option;
    if (name == "shift-option"_sv)
        return KeyboardLayout::ShiftOption;
    if (name == "caps"_sv)
        return KeyboardLayout::Caps;
    return KeyboardLayout::Plain;
}

}

KeyboardLayout KeyboardLayout::builtin()
{
    KeyboardLayout layout;
    layout.m_name = MUST(String::from_view("U.S."_sv));
    layout.m_identifier = MUST(String::from_view("US"_sv));
    for (auto const& plain : s_builtin) {
        KeyEntry entry;
        entry.code = plain.code;
        char const one[2] = { plain.plain, 0 };
        char const two[2] = { plain.shifted, 0 };
        bool const is_letter = plain.plain >= 'a' && plain.plain <= 'z';
        entry.text[Plain] = MUST(String::from_view(StringView(one, 1)));
        entry.text[Shift] = MUST(String::from_view(StringView(two, 1)));
        entry.text[Option] = entry.text[Plain];
        entry.text[ShiftOption] = entry.text[Shift];
        entry.text[Caps] = is_letter ? entry.text[Shift] : entry.text[Plain];
        layout.m_keys.append(move(entry));
    }
    return layout;
}

ErrorOr<KeyboardLayout> KeyboardLayout::load(StringView identifier)
{
    for (size_t i = 0; i < identifier.length(); ++i) {
        if (identifier[i] == '/')
            return Error::from_string_literal("not the name of a keyboard layout");
    }
    StringBuilder path;
    path.append("/System/Library/Keyboard Layouts/"_sv);
    path.append(identifier);
    path.append(".layout"_sv);
    auto const bytes = TRY(read_file(TRY(path.to_string()).view()));
    KeyboardLayout layout;
    layout.m_identifier = TRY(String::from_view(identifier));
    StringView const all { bytes.data(), bytes.size() };
    size_t start = 0;
    while (start < all.length()) {
        size_t end = start;
        while (end < all.length() && all[end] != '\n')
            ++end;
        auto const line = all.substring_view(start, end - start);
        start = end + 1;
        if (line.starts_with("name "_sv)) {
            layout.m_name = TRY(String::from_view(line.substring_view(5)));
            continue;
        }
        auto const fields = fields_of(line);
        if (fields.size() == 7 && fields[0] == "key"_sv) {
            KeyEntry entry;
            entry.code = static_cast<u16>(number_of(fields[1]));
            for (size_t layer = 0; layer < LayerCount; ++layer) {
                auto value = fields[2 + layer];
                if (value == "-"_sv) {
                    entry.text[layer] = TRY(String::from_view(""_sv));
                    continue;
                }
                if (value.starts_with("dead:"_sv)) {
                    entry.dead[layer] = true;
                    value = value.substring_view(5);
                    if (value == "-"_sv)
                        value = ""_sv;
                }
                entry.text[layer] = TRY(utf8_from_hex_utf16(value));
            }
            TRY(layout.m_keys.try_append(move(entry)));
        } else if (fields.size() == 6 && fields[0] == "compose"_sv) {
            ComposeEntry entry;
            entry.dead_layer = layer_named(fields[1]);
            entry.dead_code = static_cast<u16>(number_of(fields[2]));
            entry.code = static_cast<u16>(number_of(fields[3]));
            entry.shifted = fields[4] == "1"_sv;
            entry.text = TRY(utf8_from_hex_utf16(fields[5]));
            TRY(layout.m_compose.try_append(move(entry)));
        }
    }
    return layout;
}

KeyboardLayout::KeyEntry const* KeyboardLayout::entry_for(u16 code) const
{
    for (auto const& key : m_keys) {
        if (key.code == code)
            return &key;
    }
    return nullptr;
}

String KeyboardLayout::type(u16 code, bool shift, bool option, bool caps, bool command_or_control)
{
    char const same = always_the_same(code);
    if (same) {
        // Whatever was waiting comes out by itself first, as a Mac does.
        StringBuilder out;
        if (m_pending) {
            out.append(m_pending->text[m_pending_layer].view());
            m_pending = nullptr;
        }
        out.append(same);
        return MUST(out.to_string());
    }
    auto const* entry = entry_for(code);
    if (!entry)
        return MUST(String::from_view(""_sv));

    // With Command or Control held it's a shortcut: it's the plain key that's meant. And that's the one thing a key's character isn't in another alphabet, as shortcuts are in Latin letters.
    if (command_or_control) {
        m_pending = nullptr;
        return entry->text[shift ? Shift : Plain];
    }

    u8 layer = Plain;
    if (option)
        layer = shift ? ShiftOption : Option;
    else if (shift)
        layer = Shift;
    else if (caps)
        layer = Caps;

    if (m_pending) {
        auto const* pending = m_pending;
        u8 const pending_layer = m_pending_layer;
        m_pending = nullptr;
        for (auto const& compose : m_compose) {
            if (compose.dead_code == pending->code && compose.dead_layer == pending_layer && compose.code == code && compose.shifted == shift)
                return compose.text;
        }
        // It doesn't go with this. Both, then: unless this one's a dead key too.
        StringBuilder out;
        out.append(pending->text[pending_layer].view());
        if (entry->dead[layer]) {
            m_pending = entry;
            m_pending_layer = layer;
        } else if (!(code == 57)) {
            out.append(entry->text[layer].view());
        }
        return MUST(out.to_string());
    }
    if (entry->dead[layer]) {
        m_pending = entry;
        m_pending_layer = layer;
        return MUST(String::from_view(""_sv));
    }
    return entry->text[layer];
}
