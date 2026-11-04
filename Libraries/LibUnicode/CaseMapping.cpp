/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Tables.h>
#include <LibUnicode/Unicode.h>

namespace Unicode {

using namespace Tables;

CodePoint simple_to_upper(CodePoint code)
{
    if (code < 0x80)
        return code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code;
    auto const* entry = find_entry(simple_upper, simple_upper_count, 2, code);
    return entry ? entry[1] : code;
}

CodePoint simple_to_lower(CodePoint code)
{
    if (code < 0x80)
        return code >= 'A' && code <= 'Z' ? code - 'A' + 'a' : code;
    auto const* entry = find_entry(simple_lower, simple_lower_count, 2, code);
    return entry ? entry[1] : code;
}

CodePoint simple_to_title(CodePoint code)
{
    if (code < 0x80)
        return code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code;
    auto const* entry = find_entry(simple_title, simple_title_count, 2, code);
    return entry ? entry[1] : code;
}

CodePoint simple_fold(CodePoint code)
{
    if (code < 0x80)
        return code >= 'A' && code <= 'Z' ? code - 'A' + 'a' : code;
    auto const* entry = find_entry(Tables::simple_fold, simple_fold_count, 2, code);
    return entry ? entry[1] : code;
}

namespace {

enum class Language : u8 {
    Other,
    Turkic, // Turkish and Azeri: i has a capital of its own, with a dot.
    Lithuanian, // Which keeps a dot over a lowercase i, with an accent on it, that it would otherwise lose.
};

Language language_of(StringView locale)
{
    if (locale.length() < 2 || (locale.length() > 2 && locale[2] != '-' && locale[2] != '_'))
        return Language::Other;
    auto const code = locale.substring_view(0, 2);
    if (code == "tr"_sv || code == "az"_sv)
        return Language::Turkic;
    if (code == "lt"_sv)
        return Language::Lithuanian;
    return Language::Other;
}

ErrorOr<void> append_sequence(Vector<CodePoint>& out, u32 const* entry)
{
    for (u32 i = 0; i < entry[1]; ++i)
        TRY(out.try_append(entry[2 + i]));
    return {};
}

// The rest of SpecialCasing's conditions, which are about what's around it.

// Is there a cased letter before this, with only what doesn't count in between?
bool follows_cased_letter(Span<CodePoint const> text, size_t index)
{
    for (size_t i = index; i > 0; --i) {
        if (Unicode::is_case_ignorable(text[i - 1]))
            continue;
        return Unicode::is_cased(text[i - 1]);
    }
    return false;
}

bool precedes_cased_letter(Span<CodePoint const> text, size_t index)
{
    for (size_t i = index + 1; i < text.size(); ++i) {
        if (Unicode::is_case_ignorable(text[i]))
            continue;
        return Unicode::is_cased(text[i]);
    }
    return false;
}

// Is there a soft-dotted letter before this, with nothing that would sit on top of it in between?
bool after_soft_dotted(Span<CodePoint const> text, size_t index)
{
    for (size_t i = index; i > 0; --i) {
        if (has_property(is_soft_dotted, is_soft_dotted_count, text[i - 1]))
            return true;
        auto const combining = Unicode::combining_class(text[i - 1]);
        if (combining == 0 || combining == 230)
            return false;
    }
    return false;
}

bool more_above(Span<CodePoint const> text, size_t index)
{
    for (size_t i = index + 1; i < text.size(); ++i) {
        auto const combining = Unicode::combining_class(text[i]);
        if (combining == 230)
            return true;
        if (combining == 0)
            return false;
    }
    return false;
}

bool before_dot(Span<CodePoint const> text, size_t index)
{
    for (size_t i = index + 1; i < text.size(); ++i) {
        if (text[i] == 0x307)
            return true;
        auto const combining = Unicode::combining_class(text[i]);
        if (combining == 0 || combining == 230)
            return false;
    }
    return false;
}

bool after_i(Span<CodePoint const> text, size_t index)
{
    for (size_t i = index; i > 0; --i) {
        if (text[i - 1] == 'I')
            return true;
        auto const combining = Unicode::combining_class(text[i - 1]);
        if (combining == 0 || combining == 230)
            return false;
    }
    return false;
}

ErrorOr<void> lower_one(Vector<CodePoint>& out, Span<CodePoint const> text, size_t index, Language language)
{
    CodePoint const code = text[index];
    // A capital sigma at the end of a word.
    if (code == 0x3a3) {
        TRY(out.try_append(follows_cased_letter(text, index) && !precedes_cased_letter(text, index) ? 0x3c2 : 0x3c3));
        return {};
    }
    if (language == Language::Turkic) {
        if (code == 0x130) {
            TRY(out.try_append('i'));
            return {};
        }
        if (code == 0x307 && after_i(text, index))
            return {};
        if (code == 'I') {
            TRY(out.try_append(before_dot(text, index) ? 'i' : 0x131));
            return {};
        }
    }
    if (language == Language::Lithuanian) {
        if ((code == 'I' || code == 'J' || code == 0x12e) && more_above(text, index)) {
            TRY(out.try_append(simple_to_lower(code)));
            TRY(out.try_append(0x307));
            return {};
        }
        CodePoint marked = 0;
        CodePoint accent = 0;
        if (code == 0xcc)
            accent = 0x300;
        if (code == 0xcd)
            accent = 0x301;
        if (code == 0x128)
            accent = 0x303;
        if (accent)
            marked = 'i';
        if (marked) {
            TRY(out.try_append(marked));
            TRY(out.try_append(0x307));
            TRY(out.try_append(accent));
            return {};
        }
    }
    if (auto const* entry = find_entry(special_lower, special_lower_count, 5, code))
        return append_sequence(out, entry);
    TRY(out.try_append(simple_to_lower(code)));
    return {};
}

ErrorOr<void> upper_one(Vector<CodePoint>& out, Span<CodePoint const> text, size_t index, Language language)
{
    CodePoint const code = text[index];
    if (language == Language::Turkic && code == 'i') {
        TRY(out.try_append(0x130));
        return {};
    }
    if (language == Language::Lithuanian && code == 0x307 && after_soft_dotted(text, index))
        return {};
    if (auto const* entry = find_entry(special_upper, special_upper_count, 5, code))
        return append_sequence(out, entry);
    TRY(out.try_append(simple_to_upper(code)));
    return {};
}

ErrorOr<void> title_one(Vector<CodePoint>& out, Span<CodePoint const> text, size_t index, Language language)
{
    CodePoint const code = text[index];
    if (language == Language::Turkic && code == 'i') {
        TRY(out.try_append(0x130));
        return {};
    }
    if (language == Language::Lithuanian && code == 0x307 && after_soft_dotted(text, index))
        return {};
    if (auto const* entry = find_entry(special_title, special_title_count, 5, code))
        return append_sequence(out, entry);
    TRY(out.try_append(simple_to_title(code)));
    return {};
}

}

ErrorOr<Vector<CodePoint>> to_uppercase(Span<CodePoint const> text, StringView locale)
{
    Vector<CodePoint> out;
    TRY(out.try_ensure_capacity(text.size()));
    auto const language = language_of(locale);
    for (size_t i = 0; i < text.size(); ++i)
        TRY(upper_one(out, text, i, language));
    return out;
}

ErrorOr<Vector<CodePoint>> to_lowercase(Span<CodePoint const> text, StringView locale)
{
    Vector<CodePoint> out;
    TRY(out.try_ensure_capacity(text.size()));
    auto const language = language_of(locale);
    for (size_t i = 0; i < text.size(); ++i)
        TRY(lower_one(out, text, i, language));
    return out;
}

ErrorOr<Vector<CodePoint>> to_titlecase(Span<CodePoint const> text, StringView locale)
{
    Vector<CodePoint> out;
    TRY(out.try_ensure_capacity(text.size()));
    auto const language = language_of(locale);
    auto const boundaries = TRY(word_boundaries(text));
    for (size_t b = 0; b + 1 < boundaries.size(); ++b) {
        bool titled = false;
        for (size_t i = boundaries[b]; i < boundaries[b + 1]; ++i) {
            // The first letter of each word: what's before that, like a quote, stays as it is.
            if (!titled && Unicode::is_cased(text[i])) {
                TRY(title_one(out, text, i, language));
                titled = true;
            } else if (!titled) {
                TRY(out.try_append(text[i]));
            } else {
                TRY(lower_one(out, text, i, language));
            }
        }
    }
    return out;
}

ErrorOr<Vector<CodePoint>> case_fold(Span<CodePoint const> text, bool turkic)
{
    Vector<CodePoint> out;
    TRY(out.try_ensure_capacity(text.size()));
    for (CodePoint code : text) {
        if (turkic) {
            if (auto const* entry = find_entry(Tables::turkic_fold, turkic_fold_count, 2, code)) {
                TRY(out.try_append(entry[1]));
                continue;
            }
        }
        if (auto const* entry = find_entry(Tables::full_fold, full_fold_count, 5, code)) {
            TRY(append_sequence(out, entry));
            continue;
        }
        TRY(out.try_append(Unicode::simple_fold(code)));
    }
    return out;
}

ErrorOr<String> to_uppercase(StringView text, StringView locale)
{
    auto const code_points = TRY(code_points_of(text));
    auto const result = TRY(to_uppercase(code_points.span(), locale));
    return utf8_of(result.span());
}

ErrorOr<String> to_lowercase(StringView text, StringView locale)
{
    auto const code_points = TRY(code_points_of(text));
    auto const result = TRY(to_lowercase(code_points.span(), locale));
    return utf8_of(result.span());
}

ErrorOr<String> case_fold(StringView text)
{
    auto const code_points = TRY(code_points_of(text));
    auto const result = TRY(case_fold(code_points.span()));
    return utf8_of(result.span());
}

}
