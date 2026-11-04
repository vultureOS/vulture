/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibLocale/Locale.h>

namespace Locale {

DateFields date_fields(i64 seconds, i32 offset_seconds)
{
    seconds += offset_seconds;
    // Whole days, and what's left: rounding down, for what's before 1970.
    i64 days = seconds / 86400;
    i64 rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    DateFields fields;
    fields.hour = static_cast<int>(rest / 3600);
    fields.minute = static_cast<int>((rest % 3600) / 60);
    fields.second = static_cast<int>(rest % 60);
    fields.weekday = static_cast<int>(((days % 7) + 11) % 7);

    // From a day count to a date, in the proleptic Gregorian calendar, by 400-year cycles counted from 1 March of year 0.
    i64 const shifted = days + 719468;
    i64 const era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    i64 const day_of_era = shifted - era * 146097;
    i64 const year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    i64 const day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    i64 const month_index = (5 * day_of_year + 2) / 153;
    fields.day = static_cast<int>(day_of_year - (153 * month_index + 2) / 5 + 1);
    fields.month = static_cast<int>(month_index < 10 ? month_index + 3 : month_index - 9);
    fields.year = year_of_era + era * 400 + (fields.month <= 2 ? 1 : 0);
    return fields;
}

namespace {

void append_digit(StringBuilder& out, LocaleData const& locale, int digit)
{
    StringView digits { locale.digits };
    size_t at = 0;
    for (int i = 0; i < digit; ++i)
        at += Unicode::decode_utf8(digits.substring_view(at)).length;
    out.append(digits.substring_view(at, Unicode::decode_utf8(digits.substring_view(at)).length));
}

void append_number(StringBuilder& out, LocaleData const& locale, i64 value, int minimum_digits)
{
    if (value < 0) {
        out.append(StringView(locale.minus_sign));
        value = -value;
    }
    char digits[24];
    int count = 0;
    do {
        digits[count++] = static_cast<char>(value % 10);
        value /= 10;
    } while (value);
    for (int i = count; i < minimum_digits; ++i)
        append_digit(out, locale, 0);
    while (count)
        append_digit(out, locale, digits[--count]);
}

void append_offset(StringBuilder& out, LocaleData const& locale, i32 offset_seconds)
{
    // GMT+1, the short way, with what the locale calls it.
    if (offset_seconds == 0) {
        out.append(StringView(locale.gmt_zero_format));
        return;
    }
    StringView const format { locale.gmt_format };
    auto const place = format.find("{0}"_sv);
    out.append(format.substring_view(0, place.value_or(format.length())));
    out.append(offset_seconds < 0 ? StringView(locale.minus_sign) : StringView(locale.plus_sign));
    i32 const magnitude = offset_seconds < 0 ? -offset_seconds : offset_seconds;
    append_number(out, locale, magnitude / 3600, 1);
    if (magnitude % 3600) {
        out.append(':');
        append_number(out, locale, (magnitude % 3600) / 60, 2);
    }
    if (place.has_value())
        out.append(format.substring_view(place.value() + 3));
}

void append_field(StringBuilder& out, LocaleData const& locale, char letter, int count, DateFields const& fields, i32 offset_seconds)
{
    switch (letter) {
    case 'G':
        out.append(StringView(locale.era_ad));
        break;
    case 'y':
    case 'u':
    case 'U':
    case 'Y':
        if (count == 2)
            append_number(out, locale, ((fields.year % 100) + 100) % 100, 2);
        else
            append_number(out, locale, fields.year, count);
        break;
    case 'M':
    case 'L': {
        bool const standalone = letter == 'L';
        int const month = fields.month - 1;
        if (count <= 2)
            append_number(out, locale, fields.month, count);
        else if (count == 3)
            out.append(StringView((standalone ? locale.months_standalone_abbreviated : locale.months_format_abbreviated)[month]));
        else if (count == 4)
            out.append(StringView((standalone ? locale.months_standalone_wide : locale.months_format_wide)[month]));
        else
            out.append(StringView((standalone ? locale.months_standalone_narrow : locale.months_format_narrow)[month]));
        break;
    }
    case 'd':
        append_number(out, locale, fields.day, count);
        break;
    case 'E':
    case 'c':
    case 'e': {
        bool const standalone = letter == 'c';
        int const day = fields.weekday;
        if ((letter == 'c' || letter == 'e') && count <= 2) {
            // Which day of the week, counted from whichever it starts on.
            append_number(out, locale, ((day - locale.first_day_of_week + 7) % 7) + 1, count);
        } else if (count <= 3) {
            out.append(StringView((standalone ? locale.days_standalone_abbreviated : locale.days_format_abbreviated)[day]));
        } else if (count == 4) {
            out.append(StringView((standalone ? locale.days_standalone_wide : locale.days_format_wide)[day]));
        } else if (count == 5) {
            out.append(StringView((standalone ? locale.days_standalone_narrow : locale.days_format_narrow)[day]));
        } else {
            out.append(StringView((standalone ? locale.days_standalone_short : locale.days_format_short)[day]));
        }
        break;
    }
    case 'a':
    case 'b':
    case 'B':
        out.append(StringView(fields.hour < 12 ? locale.am : locale.pm));
        break;
    case 'h':
        append_number(out, locale, fields.hour % 12 == 0 ? 12 : fields.hour % 12, count);
        break;
    case 'H':
        append_number(out, locale, fields.hour, count);
        break;
    case 'K':
        append_number(out, locale, fields.hour % 12, count);
        break;
    case 'k':
        append_number(out, locale, fields.hour == 0 ? 24 : fields.hour, count);
        break;
    case 'm':
        append_number(out, locale, fields.minute, count);
        break;
    case 's':
        append_number(out, locale, fields.second, count);
        break;
    case 'S':
        for (int i = 0; i < count; ++i)
            append_digit(out, locale, 0);
        break;
    case 'z':
    case 'v':
    case 'V':
        if (offset_seconds == 0)
            out.append(StringView(count >= 4 ? locale.utc_long_name : locale.utc_short_name));
        else
            append_offset(out, locale, offset_seconds);
        break;
    case 'O':
        append_offset(out, locale, offset_seconds);
        break;
    case 'Z':
    case 'x':
    case 'X': {
        i32 const magnitude = offset_seconds < 0 ? -offset_seconds : offset_seconds;
        out.append(offset_seconds < 0 ? '-' : '+');
        append_number(out, locale, magnitude / 3600, 2);
        append_number(out, locale, (magnitude % 3600) / 60, 2);
        break;
    }
    default:
        // What isn't known stays as it is, so that it can be seen.
        for (int i = 0; i < count; ++i)
            out.append(letter);
        break;
    }
}

bool is_pattern_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}

ErrorOr<String> format_date_pattern(LocaleData const& locale, StringView pattern, i64 seconds, i32 offset_seconds)
{
    auto const fields = date_fields(seconds, offset_seconds);
    StringBuilder out;
    for (size_t i = 0; i < pattern.length();) {
        char const c = pattern[i];
        if (c == '\'') {
            // '' is one of them; otherwise, it's everything up to the next.
            if (i + 1 < pattern.length() && pattern[i + 1] == '\'') {
                out.append('\'');
                i += 2;
                continue;
            }
            ++i;
            while (i < pattern.length()) {
                if (pattern[i] == '\'') {
                    if (i + 1 < pattern.length() && pattern[i + 1] == '\'') {
                        out.append('\'');
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                out.append(pattern[i++]);
            }
            continue;
        }
        if (is_pattern_letter(c)) {
            size_t count = 1;
            while (i + count < pattern.length() && pattern[i + count] == c)
                ++count;
            append_field(out, locale, c, static_cast<int>(count), fields, offset_seconds);
            i += count;
            continue;
        }
        out.append(c);
        ++i;
    }
    return out.to_string();
}

ErrorOr<String> format_date(LocaleData const& locale, i64 seconds, Style date, Style time, i32 offset_seconds)
{
    auto const date_index = static_cast<size_t>(date);
    auto const time_index = static_cast<size_t>(time);
    if (date == Style::None && time == Style::None)
        return String();
    if (time == Style::None)
        return format_date_pattern(locale, StringView(locale.date_formats[date_index]), seconds, offset_seconds);
    if (date == Style::None)
        return format_date_pattern(locale, StringView(locale.time_formats[time_index]), seconds, offset_seconds);

    auto const date_text = TRY(format_date_pattern(locale, StringView(locale.date_formats[date_index]), seconds, offset_seconds));
    auto const time_text = TRY(format_date_pattern(locale, StringView(locale.time_formats[time_index]), seconds, offset_seconds));
    // The two together: how, as the longer of the date's formats is what says so.
    char const* glue = locale.date_at_time_formats[date_index];
    if (!glue)
        glue = locale.date_time_formats[date_index];
    StringView const pattern { glue };
    StringBuilder out;
    bool quoted = false;
    for (size_t i = 0; i < pattern.length(); ++i) {
        char const c = pattern[i];
        if (c == '\'') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c == '{' && i + 2 < pattern.length() && pattern[i + 2] == '}') {
            out.append(pattern[i + 1] == '0' ? time_text.view() : date_text.view());
            i += 2;
            continue;
        }
        out.append(c);
    }
    return out.to_string();
}

}
