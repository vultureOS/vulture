/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibLocale/Locale.h>

// What this system says about numbers and dates, for Meta/oracle-test.sh to compare with Tests/Parity/locale-oracle.mm, which says what a real Mac does.

extern "C" int printf(char const*, ...);

using namespace Locale;

namespace {

void say(LocaleData const& locale, char const* name, StringView text)
{
    printf("commands: BEGIN %s %s\n%.*s\ncommands: END %s %s status=0\n", locale.identifier, name, static_cast<int>(text.length()), text.characters_without_null_termination(), locale.identifier, name);
}

void say_list(LocaleData const& locale, char const* name, char const* const* values, size_t count)
{
    StringBuilder out;
    for (size_t i = 0; i < count; ++i) {
        if (i)
            out.append(" | "_sv);
        out.append(StringView(values[i]));
    }
    say(locale, name, MUST(out.to_string()).view());
}

}

int main()
{
    i64 const date = 1772719629;
    i64 const morning = 1794546000;
    char const* const style_names[] = { "full", "long", "medium", "short" };
    Style const styles[] = { Style::Full, Style::Long, Style::Medium, Style::Short };
    char const* const words[] = { "cote", "c\xc3\xb4te", "cot\xc3\xa9", "c\xc3\xb4t\xc3\xa9", "Z", "z", "a", "A", "\xc3\xb1\x61nd\xc3\xba", "nube", "nz", "zebra", "\xc3\x85ngstr\xc3\xb6m", "\xc3\x96l", "Oz", "Zoo", "\xc4\xb1", "i", "I", "\xc4\xb0", "\xd0\xb4\xd0\xbe\xd0\xbc", "\xce\xb1", "ch", "cz", "\xc5\x82", "lz", "m" };
    constexpr size_t word_count = sizeof(words) / sizeof(words[0]);

    for (size_t l = 0; l < locale_count; ++l) {
        auto const& locale = locales[l];
        char name[128];
        struct Value {
            double value;
            char const* text;
        };
        Value const numbers[] = { { 1234.5678, "1234.5678" }, { 0.5, "0.5" }, { -1234.5, "-1234.5" }, { 1234567.891, "1234567.891" }, { 12, "12" }, { 1000, "1000" } };
        for (auto const& value : numbers) {
            __builtin_snprintf(name, sizeof(name), "number %s", value.text);
            say(locale, name, MUST(format_number(locale, value.value)).view());
        }
        Value const percents[] = { { 0.256, "0.256" }, { 1.5, "1.5" } };
        for (auto const& value : percents) {
            __builtin_snprintf(name, sizeof(name), "percent %s", value.text);
            say(locale, name, MUST(format_percent(locale, value.value)).view());
        }
        char const* const codes[] = { "", "USD", "EUR", "JPY", "GBP" };
        Value const amounts[] = { { 1234.56, "1234.56" }, { -1234.56, "-1234.56" } };
        for (auto const* code : codes) {
            for (auto const& value : amounts) {
                __builtin_snprintf(name, sizeof(name), "currency %s %s", code, value.text);
                say(locale, name, MUST(format_currency(locale, value.value, StringView(code))).view());
            }
        }
        for (size_t i = 0; i < 4; ++i) {
            __builtin_snprintf(name, sizeof(name), "date %s", style_names[i]);
            say(locale, name, MUST(format_date(locale, date, styles[i], Style::None)).view());
        }
        for (size_t i = 0; i < 4; ++i) {
            __builtin_snprintf(name, sizeof(name), "time %s", style_names[i]);
            say(locale, name, MUST(format_date(locale, date, Style::None, styles[i])).view());
            __builtin_snprintf(name, sizeof(name), "morning %s", style_names[i]);
            say(locale, name, MUST(format_date(locale, morning, Style::None, styles[i])).view());
        }
        for (size_t i = 0; i < 4; ++i) {
            __builtin_snprintf(name, sizeof(name), "both %s", style_names[i]);
            say(locale, name, MUST(format_date(locale, date, styles[i], Style::Short)).view());
        }
        say_list(locale, "months", locale.months_format_wide, 12);
        say_list(locale, "short months", locale.months_format_abbreviated, 12);
        say_list(locale, "standalone months", locale.months_standalone_wide, 12);
        say_list(locale, "days", locale.days_format_wide, 7);
        say_list(locale, "short days", locale.days_format_abbreviated, 7);
        __builtin_snprintf(name, sizeof(name), "%s %s", locale.am, locale.pm);
        say(locale, "am and pm", StringView(name));
        __builtin_snprintf(name, sizeof(name), "%d", locale.first_day_of_week);
        say(locale, "first day of the week", StringView(name));

        // Sorted the way a person using that language would expect.
        auto const collator = MUST(Collator::create(StringView(locale.language)));
        char const* sorted[word_count];
        for (size_t i = 0; i < word_count; ++i) {
            sorted[i] = words[i];
            size_t j = i;
            while (j > 0 && MUST(collator.compare(StringView(sorted[j - 1]), StringView(sorted[j]))) > 0) {
                auto const* held = sorted[j - 1];
                sorted[j - 1] = sorted[j];
                sorted[j] = held;
                --j;
            }
        }
        say_list(locale, "sorted", sorted, word_count);
    }
    printf("commands: finished\n");
    return 0;
}
