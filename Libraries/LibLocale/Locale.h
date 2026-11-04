/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibLocale/Generated/Sizes.h>
#include <LibUnicode/Unicode.h>

// How the world writes numbers and dates, and sorts words: from CLDR (Generated/, made by Meta/generate-locale.py), for the locales it has.

namespace Locale {

struct LocaleData {
    char const* identifier; // en_US
    char const* language;   // en
    char const* region;     // US
    bool is_right_to_left;
    int first_day_of_week; // 0 is Sunday.
    char const* default_currency;
    char const* digits;
    int minimum_grouping_digits;
    char const* decimal_separator;
    char const* group_separator;
    char const* percent_sign;
    char const* minus_sign;
    char const* plus_sign;
    char const* infinity;
    char const* not_a_number;
    char const* decimal_pattern;
    char const* percent_pattern;
    char const* currency_pattern;
    char const* currency_spacing;
    char const* currency_symbols[currency_count];
    char const* months_format_wide[12];
    char const* months_format_abbreviated[12];
    char const* months_format_narrow[12];
    char const* days_format_wide[7];
    char const* days_format_abbreviated[7];
    char const* days_format_short[7];
    char const* days_format_narrow[7];
    char const* months_standalone_wide[12];
    char const* months_standalone_abbreviated[12];
    char const* months_standalone_narrow[12];
    char const* days_standalone_wide[7];
    char const* days_standalone_abbreviated[7];
    char const* days_standalone_short[7];
    char const* days_standalone_narrow[7];
    char const* am;
    char const* pm;
    char const* era_ad;
    char const* date_formats[4];
    char const* time_formats[4];
    char const* date_time_formats[4];
    char const* date_at_time_formats[4];
    char const* clock_format; // For the menu bar.
    char const* gmt_format;
    char const* gmt_zero_format;
    char const* hour_format;
    char const* utc_short_name;
    char const* utc_long_name;
    char const* language_names[language_name_count];
    char const* region_names[region_name_count];
};

extern LocaleData const locales[];
extern size_t const locale_count;
extern char const* const currency_codes[currency_count];
extern int const currency_decimal_places[currency_count];
extern char const* const language_codes[language_name_count];
extern char const* const region_codes[region_name_count];

// By "en_US", "en-US", or just "en": null if it's none that's here. The default is en_US, and a program should ask the system for which to use (Settings' Language &
// Region), as what's wanted isn't always where it says it is.
[[nodiscard]] LocaleData const* find_locale(StringView identifier);
[[nodiscard]] LocaleData const& default_locale();

// ---- Numbers.

[[nodiscard]] ErrorOr<String> format_number(LocaleData const&, double);
// 0.25 is 25%.
[[nodiscard]] ErrorOr<String> format_percent(LocaleData const&, double);
// By its ISO code: USD. Empty means the locale's own.
[[nodiscard]] ErrorOr<String> format_currency(LocaleData const&, double, StringView code = {});

// ---- Dates and times.

enum class Style : u8 {
    Full,
    Long,
    Medium,
    Short,
    None,
};

struct DateFields {
    i64 year { 1970 };
    int month { 1 }; // From 1.
    int day { 1 };
    int hour { 0 };
    int minute { 0 };
    int second { 0 };
    int weekday { 4 }; // 0 is Sunday.
};

[[nodiscard]] DateFields date_fields(i64 seconds_since_1970, i32 offset_seconds = 0);

// As the locale writes it. `offset_seconds` from UTC; the zone's name, for the formats that have one, is only known for UTC itself, and is otherwise GMT+1 and so on.
[[nodiscard]] ErrorOr<String> format_date(LocaleData const&, i64 seconds_since_1970, Style date, Style time, i32 offset_seconds = 0);
// By a pattern of CLDR's: EEEE, MMMM d, y.
[[nodiscard]] ErrorOr<String> format_date_pattern(LocaleData const&, StringView pattern, i64 seconds_since_1970, i32 offset_seconds = 0);

// ---- Sorting (UTS #10).

class Collator {
public:
    enum class Strength : u8 {
        Primary,    // Letters: a and á are the same, and A.
        Secondary,  // And accents: a and A are the same.
        Tertiary,   // And case.
        Quaternary, // And punctuation, if it's being ignored until now.
    };
    enum class Variable : u8 {
        NonIgnorable, // Spaces and punctuation are like letters.
        Shifted,      // They're not, until nothing else is different.
    };

    // For this language, which needn't be one with anything special about it. Empty is for what's the same everywhere (CLDR's root).
    [[nodiscard]] static ErrorOr<Collator> create(StringView language = {}, Strength = Strength::Tertiary, Variable = Variable::NonIgnorable);

    // Less than zero if `a` comes first.
    [[nodiscard]] ErrorOr<int> compare(Span<Unicode::CodePoint const> a, Span<Unicode::CodePoint const> b) const;
    [[nodiscard]] ErrorOr<int> compare(StringView a, StringView b) const;
    // What compares the same way as the text would.
    [[nodiscard]] ErrorOr<Vector<u32>> sort_key(Span<Unicode::CodePoint const>) const;

private:
    struct Element {
        u32 primary; // In 24 bits: the root's are in the upper 16 of it, and the rest is room for what a tailoring puts in between.
        u16 secondary;
        u8 tertiary;
        bool variable;
    };
    struct Tailored {
        Vector<Unicode::CodePoint> key;
        Vector<Element> elements;
    };

    Collator() = default;

    ErrorOr<void> apply_rules(StringView rules);
    ErrorOr<void> elements_of(Span<Unicode::CodePoint const> nfd, Vector<Element>&) const;
    [[nodiscard]] ErrorOr<void> elements_for_rule_text(Span<Unicode::CodePoint const> text, Vector<Element>& out) const;
    [[nodiscard]] Tailored const* longest_tailored_match(Span<Unicode::CodePoint const> text, size_t at, size_t& length) const;
    [[nodiscard]] u32 reordered(u32 primary) const;

    Strength m_strength { Strength::Tertiary };
    Variable m_variable { Variable::NonIgnorable };
    Vector<Tailored> m_tailored;
    // [reorder]: a script moved ahead of Latin. Zero if there isn't one.
    u32 m_moved_first { 0 };
    u32 m_moved_last { 0 };
    u32 m_moved_to { 0 };
};

}
