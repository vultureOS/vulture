/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Unicode.h>
#include <LibVultureFS/Names.h>

namespace VultureFS {

namespace {

// The next code point, strictly: no overlong forms, no surrogates, nothing past the last. Returns how many
// bytes it took, or 0.
size_t decode_one(StringView text, size_t at, u32& code)
{
    u8 const first = static_cast<u8>(text[at]);
    if (first < 0x80) {
        code = first;
        return 1;
    }
    size_t length;
    u32 minimum;
    if ((first & 0xe0) == 0xc0) {
        length = 2;
        code = first & 0x1f;
        minimum = 0x80;
    } else if ((first & 0xf0) == 0xe0) {
        length = 3;
        code = first & 0x0f;
        minimum = 0x800;
    } else if ((first & 0xf8) == 0xf0) {
        length = 4;
        code = first & 0x07;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (at + length > text.length())
        return 0;
    for (size_t i = 1; i < length; ++i) {
        u8 const next = static_cast<u8>(text[at + i]);
        if ((next & 0xc0) != 0x80)
            return 0;
        code = (code << 6) | (next & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return 0;
    return length;
}

ErrorOr<void> append_utf8(Vector<u8>& out, u32 code)
{
    if (code < 0x80) {
        TRY(out.try_append(static_cast<u8>(code)));
    } else if (code < 0x800) {
        TRY(out.try_append(static_cast<u8>(0xc0 | (code >> 6))));
        TRY(out.try_append(static_cast<u8>(0x80 | (code & 0x3f))));
    } else if (code < 0x10000) {
        TRY(out.try_append(static_cast<u8>(0xe0 | (code >> 12))));
        TRY(out.try_append(static_cast<u8>(0x80 | ((code >> 6) & 0x3f))));
        TRY(out.try_append(static_cast<u8>(0x80 | (code & 0x3f))));
    } else {
        TRY(out.try_append(static_cast<u8>(0xf0 | (code >> 18))));
        TRY(out.try_append(static_cast<u8>(0x80 | ((code >> 12) & 0x3f))));
        TRY(out.try_append(static_cast<u8>(0x80 | ((code >> 6) & 0x3f))));
        TRY(out.try_append(static_cast<u8>(0x80 | (code & 0x3f))));
    }
    return {};
}

}

bool is_valid_name_encoding(StringView name)
{
    for (size_t at = 0; at < name.length();) {
        u32 code;
        size_t const length = decode_one(name, at, code);
        if (!length)
            return false;
        at += length;
    }
    return true;
}

size_t name_length_in_utf16_units(StringView name)
{
    size_t units = 0;
    for (size_t at = 0; at < name.length();) {
        u32 code;
        size_t const length = decode_one(name, at, code);
        if (!length)
            return name.length();
        at += length;
        units += code >= 0x10000 ? 2 : 1;
    }
    return units;
}

ErrorOr<void> fold_name(StringView name, bool case_insensitive, Vector<u8>& folded)
{
    folded.clear();
    // Most names are plain, and this is done on every step of every path.
    bool plain = true;
    for (size_t i = 0; i < name.length() && plain; ++i)
        plain = static_cast<u8>(name[i]) < 0x80;
    if (plain) {
        TRY(folded.try_resize(name.length()));
        for (size_t i = 0; i < name.length(); ++i) {
            char const c = name[i];
            folded[i] = static_cast<u8>(case_insensitive && c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        return {};
    }
    Vector<Unicode::CodePoint> codes;
    for (size_t at = 0; at < name.length();) {
        u32 code;
        size_t const length = decode_one(name, at, code);
        // (Not a name anything can have made. It's only to be compared, and it won't be equal to anything.)
        if (!length) {
            code = 0xfffd;
            ++at;
        } else {
            at += length;
        }
        TRY(codes.try_append(code));
    }
    auto decomposed = TRY(Unicode::normalize(codes.span(), Unicode::NormalizationForm::NFD));
    if (case_insensitive) {
        // As Unicode has it for comparing canonically and without regard to case: folding can undo a decomposition.
        auto const case_folded = TRY(Unicode::case_fold(decomposed.span(), false));
        decomposed = TRY(Unicode::normalize(case_folded.span(), Unicode::NormalizationForm::NFD));
    }
    for (auto code : decomposed)
        TRY(append_utf8(folded, code));
    return {};
}

}
