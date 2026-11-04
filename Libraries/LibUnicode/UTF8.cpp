/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Unicode.h>

namespace Unicode {

Decoded decode_utf8(StringView text)
{
    auto const byte_at = [&](size_t index) { return static_cast<u8>(text[index]); };
    u8 const lead = byte_at(0);
    if (lead < 0x80)
        return { lead, 1 };
    // What each lead byte is the start of, and the narrower range its second byte has to be in, so that nothing's encoded longer than it need be, or as a surrogate, or
    // past the end of Unicode.
    size_t length = 0;
    u8 low = 0x80;
    u8 high = 0xbf;
    CodePoint value = 0;
    if (lead >= 0xc2 && lead <= 0xdf) {
        length = 2;
        value = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        length = 3;
        value = lead & 0x0f;
        if (lead == 0xe0)
            low = 0xa0;
        if (lead == 0xed)
            high = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        length = 4;
        value = lead & 0x07;
        if (lead == 0xf0)
            low = 0x90;
        if (lead == 0xf4)
            high = 0x8f;
    } else {
        return { replacement_character, 1 };
    }
    for (size_t i = 1; i < length; ++i) {
        if (i >= text.length())
            return { replacement_character, i };
        u8 const byte = byte_at(i);
        if (byte < low || byte > high)
            return { replacement_character, i };
        value = (value << 6) | (byte & 0x3f);
        low = 0x80;
        high = 0xbf;
    }
    return { value, length };
}

bool is_valid_utf8(StringView text)
{
    while (!text.is_empty()) {
        auto const decoded = decode_utf8(text);
        // (Only a real one of these takes three bytes.)
        if (decoded.code_point == replacement_character && decoded.length != 3)
            return false;
        text = text.substring_view(decoded.length);
    }
    return true;
}

size_t encode_utf8(CodePoint code, char out[4])
{
    if (code < 0x80) {
        out[0] = static_cast<char>(code);
        return 1;
    }
    if (code < 0x800) {
        out[0] = static_cast<char>(0xc0 | (code >> 6));
        out[1] = static_cast<char>(0x80 | (code & 0x3f));
        return 2;
    }
    if (code < 0x10000) {
        if (code >= 0xd800 && code <= 0xdfff)
            return 0;
        out[0] = static_cast<char>(0xe0 | (code >> 12));
        out[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out[2] = static_cast<char>(0x80 | (code & 0x3f));
        return 3;
    }
    if (code > last_code_point)
        return 0;
    out[0] = static_cast<char>(0xf0 | (code >> 18));
    out[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3f));
    out[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    out[3] = static_cast<char>(0x80 | (code & 0x3f));
    return 4;
}

ErrorOr<Vector<CodePoint>> code_points_of(StringView text)
{
    Vector<CodePoint> result;
    TRY(result.try_ensure_capacity(text.length()));
    while (!text.is_empty()) {
        auto const decoded = decode_utf8(text);
        TRY(result.try_append(decoded.code_point));
        text = text.substring_view(decoded.length);
    }
    return result;
}

ErrorOr<String> utf8_of(Span<CodePoint const> code_points)
{
    StringBuilder out;
    for (CodePoint code : code_points) {
        char bytes[4];
        size_t length = encode_utf8(code, bytes);
        if (length == 0)
            length = encode_utf8(replacement_character, bytes);
        for (size_t i = 0; i < length; ++i)
            out.append(bytes[i]);
    }
    return out.to_string();
}

}
