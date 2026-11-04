/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Tables.h>
#include <LibUnicode/Unicode.h>

namespace Unicode {

using namespace Tables;

namespace {

// Hangul syllables are worked out, not listed: there are eleven thousand of them, all regular.
constexpr CodePoint hangul_s_base = 0xac00;
constexpr CodePoint hangul_l_base = 0x1100;
constexpr CodePoint hangul_v_base = 0x1161;
constexpr CodePoint hangul_t_base = 0x11a7;
constexpr u32 hangul_l_count = 19;
constexpr u32 hangul_v_count = 21;
constexpr u32 hangul_t_count = 28;
constexpr u32 hangul_n_count = hangul_v_count * hangul_t_count;
constexpr u32 hangul_s_count = hangul_l_count * hangul_n_count;

constexpr u32 compatibility_flag = 0x80000000;

// What it decomposes to, if anything. The key has a flag in it, so this is a search of its own.
u32 const* find_decomposition(CodePoint code)
{
    size_t low = 0;
    size_t high = decompositions_count / 2;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        u32 const key = decompositions[middle * 2] & ~compatibility_flag;
        if (key == code)
            return decompositions + middle * 2;
        if (key < code)
            low = middle + 1;
        else
            high = middle;
    }
    return nullptr;
}

ErrorOr<void> decompose_into(Vector<CodePoint>& out, CodePoint code, bool compatibility)
{
    if (code >= hangul_s_base && code < hangul_s_base + hangul_s_count) {
        u32 const index = code - hangul_s_base;
        TRY(out.try_append(hangul_l_base + index / hangul_n_count));
        TRY(out.try_append(hangul_v_base + (index % hangul_n_count) / hangul_t_count));
        if (index % hangul_t_count)
            TRY(out.try_append(hangul_t_base + index % hangul_t_count));
        return {};
    }
    // Nothing below this decomposes.
    if (code >= 0xa0) {
        if (auto const* entry = find_decomposition(code); entry && (compatibility || !(entry[0] & compatibility_flag))) {
            u32 const start = entry[1] >> 8;
            u32 const length = entry[1] & 0xff;
            for (u32 i = 0; i < length; ++i)
                TRY(decompose_into(out, decomposition_sequences[start + i], compatibility));
            return {};
        }
    }
    TRY(out.try_append(code));
    return {};
}

// Each group of what combines goes in order of how it combines, stably: which is what makes two ways of writing the same thing come out the same.
void put_marks_in_order(Vector<CodePoint>& text)
{
    size_t i = 0;
    while (i < text.size()) {
        if (Unicode::combining_class(text[i]) == 0) {
            ++i;
            continue;
        }
        size_t const start = i;
        while (i < text.size() && Unicode::combining_class(text[i]) != 0)
            ++i;
        for (size_t a = start + 1; a < i; ++a) {
            CodePoint const held = text[a];
            u8 const held_class = Unicode::combining_class(held);
            size_t b = a;
            while (b > start && Unicode::combining_class(text[b - 1]) > held_class) {
                text[b] = text[b - 1];
                --b;
            }
            text[b] = held;
        }
    }
}

// The one character these two can be written as, if there is one.
CodePoint composition_of(CodePoint first, CodePoint second)
{
    if (first >= hangul_l_base && first < hangul_l_base + hangul_l_count && second >= hangul_v_base && second < hangul_v_base + hangul_v_count)
        return hangul_s_base + ((first - hangul_l_base) * hangul_v_count + (second - hangul_v_base)) * hangul_t_count;
    if (first >= hangul_s_base && first < hangul_s_base + hangul_s_count && (first - hangul_s_base) % hangul_t_count == 0 && second > hangul_t_base && second < hangul_t_base + hangul_t_count)
        return first + (second - hangul_t_base);
    size_t low = 0;
    size_t high = compositions_count / 3;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        u32 const a = compositions[middle * 3];
        u32 const b = compositions[middle * 3 + 1];
        if (a == first && b == second)
            return compositions[middle * 3 + 2];
        if (a < first || (a == first && b < second))
            low = middle + 1;
        else
            high = middle;
    }
    return 0;
}

void compose(Vector<CodePoint>& text)
{
    if (text.is_empty())
        return;
    size_t written = 0;
    size_t starter = text.size(); // None yet.
    i32 last_class = -1;
    for (size_t i = 0; i < text.size(); ++i) {
        CodePoint const code = text[i];
        u8 const current_class = Unicode::combining_class(code);
        // A mark can join what's before it unless something of the same class or a starter's in between.
        bool const blocked = last_class != -1 && (last_class >= current_class);
        if (starter != text.size() && !blocked) {
            if (CodePoint const composed = composition_of(text[starter], code)) {
                text[starter] = composed;
                continue;
            }
        }
        if (current_class == 0) {
            starter = written;
            last_class = -1;
        } else {
            last_class = current_class;
        }
        text[written++] = code;
    }
    MUST(text.try_resize(written));
}

}

CodePoint compose_pair(CodePoint first, CodePoint second)
{
    return composition_of(first, second);
}

ErrorOr<void> append_canonical_decomposition(Vector<CodePoint>& out, CodePoint code)
{
    return decompose_into(out, code, false);
}

ErrorOr<Vector<CodePoint>> normalize(Span<CodePoint const> input, NormalizationForm form)
{
    bool const compatibility = form == NormalizationForm::NFKD || form == NormalizationForm::NFKC;
    Vector<CodePoint> text;
    TRY(text.try_ensure_capacity(input.size()));
    for (CodePoint code : input)
        TRY(decompose_into(text, code, compatibility));
    put_marks_in_order(text);
    if (form == NormalizationForm::NFC || form == NormalizationForm::NFKC)
        compose(text);
    return text;
}

ErrorOr<String> normalize(StringView utf8, NormalizationForm form)
{
    auto const code_points = TRY(code_points_of(utf8));
    auto const result = TRY(normalize(code_points.span(), form));
    return utf8_of(result.span());
}

ErrorOr<bool> is_normalized(StringView utf8, NormalizationForm form)
{
    auto const normalized = TRY(normalize(utf8, form));
    return normalized.view() == utf8;
}

}
