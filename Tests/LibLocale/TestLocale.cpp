/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCompress/Deflate.h>
#include <LibLocale/Locale.h>
#include <LibTest/TestCase.h>

// Unicode's own test of sorting, every line of it; and a few of what a person would notice. Tests/Parity/locale-oracle.mm is what compares the rest with a real Mac.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
int printf(char const*, ...);
}

using namespace Locale;
using Unicode::CodePoint;

namespace {

Vector<u8> read_gzipped(StringView name)
{
    StringBuilder path;
    path.append(StringView(VULTURE_LOCALE_TEST_DATA));
    path.append(name);
    path.append(".gz"_sv);
    auto const full_path = MUST(path.to_string());
    FILE* file = fopen(full_path.characters(), "rb");
    VERIFY(file);
    Vector<u8> bytes;
    u8 chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t const at = bytes.size();
        MUST(bytes.try_resize(at + got));
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return MUST(Compress::gzip_decompress(bytes.span()));
}

u32 hex_value(StringView text)
{
    u32 value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        char const c = text[i];
        value = value * 16 + static_cast<u32>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return value;
}

void check_order(StringView file, Collator const& collator)
{
    auto const data = read_gzipped(file);
    StringView const all { reinterpret_cast<char const*>(data.data()), data.size() };
    Vector<CodePoint> previous;
    Vector<u32> previous_key;
    bool have_previous = false;
    size_t passed = 0;
    size_t failed = 0;
    size_t start = 0;
    size_t number = 0;
    while (start < all.length()) {
        size_t end = start;
        while (end < all.length() && all[end] != '\n')
            ++end;
        ++number;
        auto line = all.substring_view(start, end - start);
        start = end + 1;
        if (auto const comment = line.find('#'); comment.has_value())
            line = line.substring_view(0, comment.value());
        Vector<CodePoint> text;
        size_t i = 0;
        while (i < line.length()) {
            while (i < line.length() && (line[i] == ' ' || line[i] == ';' || line[i] == '\t'))
                ++i;
            size_t const first = i;
            while (i < line.length() && line[i] != ' ' && line[i] != ';' && line[i] != '\t')
                ++i;
            if (i > first)
                MUST(text.try_append(hex_value(line.substring_view(first, i - first))));
        }
        if (text.is_empty())
            continue;
        auto key = MUST(collator.sort_key(text.span()));
        if (have_previous) {
            int order = 0;
            size_t const shared = min(previous_key.size(), key.size());
            for (size_t n = 0; n < shared && order == 0; ++n) {
                if (previous_key[n] != key[n])
                    order = previous_key[n] < key[n] ? -1 : 1;
            }
            if (order == 0 && previous_key.size() != key.size())
                order = previous_key.size() < key.size() ? -1 : 1;
            if (order > 0) {
                ++failed;
                if (failed <= 8)
                    printf("%.*s: line %zu comes before the one above it\n", static_cast<int>(file.length()), file.characters_without_null_termination(), number);
            } else {
                ++passed;
            }
        }
        previous = move(text);
        previous_key = move(key);
        have_previous = true;
    }
    printf("%.*s: %zu of %zu\n", static_cast<int>(file.length()), file.characters_without_null_termination(), passed, passed + failed);
    EXPECT_EQ(failed, 0u);
    EXPECT(passed > 100000);
}

Vector<CodePoint> points(StringView text)
{
    return MUST(Unicode::code_points_of(text));
}

int compared(Collator const& collator, StringView a, StringView b)
{
    return MUST(collator.compare(a, b));
}

}

TEST_CASE(sorting_conformance_non_ignorable)
{
    check_order("CollationTest_CLDR_NON_IGNORABLE_SHORT.txt"_sv, MUST(Collator::create({}, Collator::Strength::Tertiary, Collator::Variable::NonIgnorable)));
}

TEST_CASE(sorting_conformance_shifted)
{
    check_order("CollationTest_CLDR_SHIFTED_SHORT.txt"_sv, MUST(Collator::create({}, Collator::Strength::Quaternary, Collator::Variable::Shifted)));
}

TEST_CASE(sorting_for_people)
{
    auto const root = MUST(Collator::create());
    EXPECT(compared(root, "a"_sv, "b"_sv) < 0);
    EXPECT(compared(root, "a"_sv, "A"_sv) < 0);
    EXPECT(compared(root, "A"_sv, "b"_sv) < 0);
    EXPECT(compared(root, "z"_sv, "\xc3\xa1"_sv) > 0);
    EXPECT(compared(root, "e\xcc\x81"_sv, "\xc3\xa9"_sv) == 0);
    EXPECT(compared(root, "9"_sv, "a"_sv) < 0);
    EXPECT(compared(root, "\xce\xb1"_sv, "\xd0\xb0"_sv) < 0);
    EXPECT(compared(root, "a"_sv, "\xd0\xb0"_sv) < 0);
    EXPECT(compared(root, "\xe4\xb8\x80"_sv, "z"_sv) > 0);

    // Strength.
    auto const primary = MUST(Collator::create({}, Collator::Strength::Primary));
    EXPECT(compared(primary, "resume"_sv, "R\xc3\xa9sum\xc3\xa9"_sv) == 0);
    auto const secondary = MUST(Collator::create({}, Collator::Strength::Secondary));
    EXPECT(compared(secondary, "resume"_sv, "RESUME"_sv) == 0);
    EXPECT(compared(secondary, "resume"_sv, "r\xc3\xa9sum\xc3\xa9"_sv) < 0);

    // Punctuation, ignored until nothing else is different.
    auto const shifted = MUST(Collator::create({}, Collator::Strength::Tertiary, Collator::Variable::Shifted));
    EXPECT(compared(shifted, "de luge"_sv, "deluge"_sv) == 0);
    auto const not_shifted = MUST(Collator::create());
    EXPECT(compared(not_shifted, "de luge"_sv, "deluge"_sv) < 0);
}

TEST_CASE(sorting_in_a_language)
{
    // In Spanish, ñ is a letter after n. Everywhere else, it's an n with something on it.
    auto const spanish = MUST(Collator::create("es_ES"_sv));
    auto const root = MUST(Collator::create());
    EXPECT(compared(spanish, "\xc3\xb1"_sv, "nz"_sv) > 0);
    EXPECT(compared(root, "\xc3\xb1"_sv, "nz"_sv) < 0);
    EXPECT(compared(spanish, "\xc3\xb1"_sv, "o"_sv) < 0);
    EXPECT(compared(spanish, "\xc3\xb1"_sv, "\xc3\x91"_sv) < 0);
    // Decomposed, the same.
    EXPECT(compared(spanish, "n\xcc\x83"_sv, "\xc3\xb1"_sv) == 0);

    // In Swedish, å ä ö are after z.
    auto const swedish = MUST(Collator::create("sv"_sv));
    EXPECT(compared(swedish, "\xc3\xa5"_sv, "z"_sv) > 0);
    EXPECT(compared(swedish, "\xc3\xa4"_sv, "\xc3\xa5"_sv) > 0);
    EXPECT(compared(swedish, "\xc3\xb6"_sv, "\xc3\xa4"_sv) > 0);
    EXPECT(compared(root, "\xc3\xa5"_sv, "z"_sv) < 0);

    // In Turkish, ı comes before i, and they're different letters.
    auto const turkish = MUST(Collator::create("tr"_sv));
    EXPECT(compared(turkish, "\xc4\xb1"_sv, "i"_sv) < 0);
    EXPECT(compared(turkish, "h"_sv, "\xc4\xb1"_sv) < 0);
    EXPECT(compared(turkish, "\xc3\xa7"_sv, "cz"_sv) > 0);

    // In Polish, ł is a letter.
    auto const polish = MUST(Collator::create("pl"_sv));
    EXPECT(compared(polish, "\xc5\x82"_sv, "lz"_sv) > 0);
    EXPECT(compared(polish, "\xc5\x82"_sv, "m"_sv) < 0);

    // In Russian, Cyrillic comes first.
    auto const russian = MUST(Collator::create("ru"_sv));
    EXPECT(compared(russian, "\xd0\xb0"_sv, "a"_sv) < 0);
    EXPECT(compared(root, "\xd0\xb0"_sv, "a"_sv) > 0);
    EXPECT(compared(russian, "\xd0\xb1"_sv, "\xd0\xb0"_sv) > 0);
    (void)points;
}

TEST_CASE(numbers)
{
    auto const& us = *find_locale("en_US"_sv);
    auto const& germany = *find_locale("de-DE"_sv);
    auto const& france = *find_locale("fr"_sv);
    auto const& india = *find_locale("hi_IN"_sv);
    auto const& spain = *find_locale("es_ES"_sv);
    EXPECT(MUST(format_number(us, 1234.5678)).view() == "1,234.568"_sv);
    EXPECT(MUST(format_number(germany, 1234.5678)).view() == "1.234,568"_sv);
    EXPECT(MUST(format_number(france, 1234.5678)).view() == "1\xe2\x80\xaf" "234,568"_sv);
    EXPECT(MUST(format_number(india, 1234567.891)).view() == "12,34,567.891"_sv);
    // Spanish doesn't group until there's more of it.
    EXPECT(MUST(format_number(spain, 1234)).view() == "1234"_sv);
    EXPECT(MUST(format_number(spain, 12345)).view() == "12.345"_sv);
    EXPECT(MUST(format_number(us, -5)).view() == "-5"_sv);
    EXPECT(MUST(format_percent(us, 0.25)).view() == "25%"_sv);
    EXPECT(MUST(format_currency(us, 1234.5)).view() == "$1,234.50"_sv);
    EXPECT(MUST(format_currency(us, -1234.5)).view() == "-$1,234.50"_sv);
    EXPECT(MUST(format_currency(germany, 1234.5, "EUR"_sv)).view() == "1.234,50\xc2\xa0\xe2\x82\xac"_sv);
    EXPECT(MUST(format_currency(us, 1234.6, "JPY"_sv)).view() == "\xc2\xa5" "1,235"_sv);
    EXPECT(find_locale("xx"_sv) == nullptr);
    EXPECT(StringView(find_locale("de"_sv)->identifier) == "de_DE"_sv);
}

TEST_CASE(dates)
{
    auto const& us = *find_locale("en_US"_sv);
    auto const& germany = *find_locale("de_DE"_sv);
    i64 const when = 1772719629; // 2026-03-05 14:07:09 UTC.
    auto const fields = date_fields(when);
    EXPECT_EQ(fields.year, 2026);
    EXPECT_EQ(fields.month, 3);
    EXPECT_EQ(fields.day, 5);
    EXPECT_EQ(fields.hour, 14);
    EXPECT_EQ(fields.minute, 7);
    EXPECT_EQ(fields.second, 9);
    EXPECT_EQ(fields.weekday, 4);
    auto const before_1970 = date_fields(-1);
    EXPECT_EQ(before_1970.year, 1969);
    EXPECT_EQ(before_1970.month, 12);
    EXPECT_EQ(before_1970.day, 31);
    EXPECT_EQ(before_1970.hour, 23);
    EXPECT_EQ(before_1970.weekday, 3);
    auto const leap_day = date_fields(951782400); // 2000-02-29.
    EXPECT_EQ(leap_day.month, 2);
    EXPECT_EQ(leap_day.day, 29);

    EXPECT(MUST(format_date(us, when, Style::Full, Style::None)).view() == "Thursday, March 5, 2026"_sv);
    EXPECT(MUST(format_date(germany, when, Style::Full, Style::None)).view() == "Donnerstag, 5. M\xc3\xa4rz 2026"_sv);
    EXPECT(MUST(format_date(germany, when, Style::None, Style::Short)).view() == "14:07"_sv);
    EXPECT(MUST(format_date_pattern(us, "yy/M/d 'at' HH:mm:ss"_sv, when)).view() == "26/3/5 at 14:07:09"_sv);
    EXPECT(MUST(format_date_pattern(us, "h 'o''clock' a"_sv, when)).view() == "2 o'clock PM"_sv);
    EXPECT(MUST(format_date_pattern(us, "HH O"_sv, when, 3600)).view() == "15 GMT+1"_sv);
    EXPECT(MUST(format_date_pattern(us, "Z"_sv, when, -5 * 3600)).view() == "-0500"_sv);
}
