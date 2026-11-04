/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibTest/TestCase.h>
#include <LibUnicode/Unicode.h>

using namespace Unicode;

TEST_CASE(properties)
{
    EXPECT(general_category('A') == GeneralCategory::Lu);
    EXPECT(general_category('a') == GeneralCategory::Ll);
    EXPECT(general_category('1') == GeneralCategory::Nd);
    EXPECT(general_category(' ') == GeneralCategory::Zs);
    EXPECT(general_category(0x20ac) == GeneralCategory::Sc);
    EXPECT(general_category(0x4e00) == GeneralCategory::Lo);
    EXPECT(general_category(0xac00) == GeneralCategory::Lo);
    EXPECT(general_category(0xd800) == GeneralCategory::Cs);
    EXPECT(general_category(0xe000) == GeneralCategory::Co);
    EXPECT(general_category(0x378) == GeneralCategory::Cn);
    EXPECT(general_category(0x10ffff) == GeneralCategory::Cn);
    EXPECT(is_alphabetic(0x3b1));
    EXPECT(!is_alphabetic('1'));
    EXPECT(is_uppercase(0x391));
    EXPECT(is_lowercase(0x3b1));
    EXPECT(is_white_space(0x3000));
    EXPECT(is_emoji(0x1f600));
    EXPECT(is_extended_pictographic(0x1f600));
    EXPECT(!is_emoji('A'));
    EXPECT(is_ideographic(0x4e00));
    EXPECT_EQ(decimal_digit_value('7'), 7);
    EXPECT_EQ(decimal_digit_value(0x667), 7); // Arabic-Indic.
    EXPECT_EQ(decimal_digit_value('a'), -1);
    EXPECT(script_name(script_of('A')) == "Latin"_sv);
    EXPECT(script_name(script_of(0x3b1)) == "Greek"_sv);
    EXPECT(script_name(script_of(0x627)) == "Arabic"_sv);
    EXPECT(script_name(script_of(0x4e00)) == "Han"_sv);
    EXPECT(script_name(script_of(0xac00)) == "Hangul"_sv);
    EXPECT(script_name(script_of(0x378)) == "Unknown"_sv);
    EXPECT(script_name(script_of(' ')) == "Common"_sv);
    EXPECT_EQ(combining_class(0x301), 230);
    EXPECT_EQ(combining_class('a'), 0);
    EXPECT_EQ(combining_class(0x323), 220);
}

TEST_CASE(column_widths)
{
    EXPECT_EQ(column_width('a'), 1);
    EXPECT_EQ(column_width(0x4e00), 2);
    EXPECT_EQ(column_width(0xff21), 2); // Fullwidth A.
    EXPECT_EQ(column_width(0x1f600), 2);
    EXPECT_EQ(column_width(0x301), 0);
    EXPECT_EQ(column_width(0x200d), 0);
    EXPECT_EQ(column_width(0), 0);
    EXPECT_EQ(column_width(7), -1);
    EXPECT_EQ(column_width(0x378), -1);
    EXPECT_EQ(column_width(0x1160), 0);
    EXPECT_EQ(column_width(0x1100), 2);
}

TEST_CASE(utf8)
{
    auto const decoded = [](StringView text) { return decode_utf8(text); };
    EXPECT_EQ(decoded("a"_sv).code_point, static_cast<CodePoint>('a'));
    EXPECT_EQ(decoded("\xc3\xa9"_sv).code_point, 0xe9u);
    EXPECT_EQ(decoded("\xc3\xa9"_sv).length, 2u);
    EXPECT_EQ(decoded("\xe2\x82\xac"_sv).code_point, 0x20acu);
    EXPECT_EQ(decoded("\xf0\x9f\x98\x80"_sv).code_point, 0x1f600u);
    EXPECT_EQ(decoded("\xf0\x9f\x98\x80"_sv).length, 4u);
    // What isn't right is one replacement for as much as could have been the start of something.
    EXPECT_EQ(decoded("\xc0\x80"_sv).code_point, replacement_character);
    EXPECT_EQ(decoded("\xc0\x80"_sv).length, 1u);
    EXPECT_EQ(decoded("\xed\xa0\x80"_sv).code_point, replacement_character); // A surrogate.
    EXPECT_EQ(decoded("\xed\xa0\x80"_sv).length, 1u);
    EXPECT_EQ(decoded("\xf4\x90\x80\x80"_sv).length, 1u); // Past the end of Unicode.
    EXPECT_EQ(decoded("\xe2\x82"_sv).length, 2u); // Cut short.
    EXPECT_EQ(decoded("\xe2\x28\xa1"_sv).length, 1u);
    EXPECT(is_valid_utf8("h\xc3\xa9llo \xf0\x9f\x98\x80"_sv));
    EXPECT(is_valid_utf8("\xef\xbf\xbd"_sv)); // A real one.
    EXPECT(!is_valid_utf8("\xff"_sv));
    EXPECT(!is_valid_utf8("\xc3"_sv));
    char bytes[4];
    EXPECT_EQ(encode_utf8(0x10ffff, bytes), 4u);
    EXPECT_EQ(encode_utf8(0x110000, bytes), 0u);
    EXPECT_EQ(encode_utf8(0xd800, bytes), 0u);
    auto const round_trip = MUST(utf8_of(MUST(code_points_of("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"_sv)).span()));
    EXPECT(round_trip.view() == "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"_sv);
}

TEST_CASE(case_mapping)
{
    auto const upper = [](StringView text, StringView locale = {}) { return MUST(to_uppercase(text, locale)); };
    auto const lower = [](StringView text, StringView locale = {}) { return MUST(to_lowercase(text, locale)); };
    EXPECT(upper("hello"_sv).view() == "HELLO"_sv);
    EXPECT(lower("HELLO"_sv).view() == "hello"_sv);
    // One to many.
    EXPECT(upper("stra\xc3\x9f" "e"_sv).view() == "STRASSE"_sv);
    EXPECT(upper("\xef\xac\x81"_sv).view() == "FI"_sv); // The fi ligature.
    // A sigma, at the end of a word and not.
    EXPECT(lower("\xce\xa3\xce\x91\xce\xa3"_sv).view() == "\xcf\x83\xce\xb1\xcf\x82"_sv);
    EXPECT(lower("\xce\xa3"_sv).view() == "\xcf\x83"_sv);
    EXPECT(lower("A\xce\xa3 B"_sv).view() == "a\xcf\x82 b"_sv);
    // Turkish has two i's.
    EXPECT(upper("i"_sv).view() == "I"_sv);
    EXPECT(upper("i"_sv, "tr"_sv).view() == "\xc4\xb0"_sv);
    EXPECT(lower("I"_sv, "tr-TR"_sv).view() == "\xc4\xb1"_sv);
    EXPECT(lower("\xc4\xb0"_sv, "tr"_sv).view() == "i"_sv);
    EXPECT(lower("\xc4\xb0"_sv).view() == "i\xcc\x87"_sv);
    // Lithuanian keeps its dot.
    EXPECT(lower("\xc3\x8c"_sv, "lt"_sv).view() == "i\xcc\x87\xcc\x80"_sv);
    EXPECT(MUST(case_fold("Stra\xc3\x9f" "e"_sv)).view() == "strasse"_sv);
    EXPECT(MUST(case_fold("\xce\xa3"_sv)).view() == MUST(case_fold("\xcf\x82"_sv)).view());

    auto const code_points = MUST(code_points_of("hello wORLD, it's"_sv));
    auto const titled = MUST(utf8_of(MUST(to_titlecase(code_points.span())).span()));
    EXPECT(titled.view() == "Hello World, It's"_sv);
    EXPECT_EQ(simple_to_upper(0xdf), 0xdfu);
    EXPECT_EQ(simple_to_title(0x1c6), 0x1c5u);
}

TEST_CASE(normalized_strings)
{
    EXPECT(MUST(normalize("e\xcc\x81"_sv, NormalizationForm::NFC)).view() == "\xc3\xa9"_sv);
    EXPECT(MUST(normalize("\xc3\xa9"_sv, NormalizationForm::NFD)).view() == "e\xcc\x81"_sv);
    EXPECT(MUST(normalize("\xef\xac\x81"_sv, NormalizationForm::NFKC)).view() == "fi"_sv);
    EXPECT(MUST(normalize("\xef\xac\x81"_sv, NormalizationForm::NFC)).view() == "\xef\xac\x81"_sv);
    // Hangul.
    EXPECT(MUST(normalize("\xea\xb0\x80"_sv, NormalizationForm::NFD)).view() == "\xe1\x84\x80\xe1\x85\xa1"_sv);
    EXPECT(MUST(normalize("\xe1\x84\x80\xe1\x85\xa1"_sv, NormalizationForm::NFC)).view() == "\xea\xb0\x80"_sv);
    EXPECT(MUST(is_normalized("\xc3\xa9"_sv, NormalizationForm::NFC)));
    EXPECT(!MUST(is_normalized("e\xcc\x81"_sv, NormalizationForm::NFC)));
    // Nothing at all.
    EXPECT(MUST(normalize(""_sv, NormalizationForm::NFC)).is_empty());
}

TEST_CASE(moving_by_characters)
{
    auto const text = MUST(code_points_of("e\xcc\x81x\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xf0\x9f\x87\xac\xf0\x9f\x87\xa7\xf0\x9f\x87\xba\xf0\x9f\x87\xb8"_sv));
    size_t const expected[] = { 2, 3, 6, 8, 10 };
    size_t at = 0;
    for (size_t next : expected) {
        at = next_grapheme_boundary(text.span(), at);
        EXPECT_EQ(at, next);
    }
    EXPECT_EQ(next_grapheme_boundary(text.span(), at), text.size());
}

TEST_CASE(reading_order)
{
    // "abc" and then Hebrew, in a paragraph that starts left to right: the Hebrew's backwards.
    auto const text = MUST(code_points_of("ab \xd7\x90\xd7\x91"_sv));
    auto const paragraph = MUST(resolve_bidi(text.span()));
    EXPECT_EQ(paragraph.paragraph_level, 0);
    auto const levels = MUST(reordering_levels(paragraph, 0, text.size()));
    auto const order = MUST(visual_order(levels.span()));
    size_t const expected[] = { 0, 1, 2, 4, 3 };
    for (size_t i = 0; i < 5; ++i)
        EXPECT_EQ(order[i], expected[i]);
    EXPECT_EQ(bidi_mirrored('('), static_cast<CodePoint>(')'));
    EXPECT_EQ(bidi_mirrored('a'), static_cast<CodePoint>('a'));
}

TEST_CASE(line_breaks_in_words)
{
    auto const text = MUST(code_points_of("Hello world.\nA\xc2\xa0" "b"_sv));
    auto const opportunities = MUST(line_break_opportunities(text.span()));
    EXPECT_EQ(opportunities.size(), 3u);
    EXPECT_EQ(opportunities[0].position, 6u);
    EXPECT(!opportunities[0].mandatory);
    EXPECT_EQ(opportunities[1].position, 13u);
    EXPECT(opportunities[1].mandatory);
    EXPECT_EQ(opportunities[2].position, text.size());
    EXPECT(MUST(line_break_opportunities(Span<CodePoint const> {})).is_empty());
}
