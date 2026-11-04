/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibLocale/Locale.h>

namespace Locale {

namespace {

struct NumberPattern {
    StringView positive_prefix;
    StringView positive_suffix;
    StringView negative_prefix;
    StringView negative_suffix;
    bool has_negative { false };
    int minimum_integer_digits { 1 };
    int minimum_fraction_digits { 0 };
    int maximum_fraction_digits { 0 };
    int primary_group { 0 };
    int secondary_group { 0 };
};

bool is_number_character(char c)
{
    return c == '#' || c == '0' || c == ',' || c == '.';
}

// What's before and after the digits, and the digits' shape from what's in the middle.
void split_part(StringView part, StringView& prefix, StringView& number, StringView& suffix)
{
    size_t start = 0;
    bool quoted = false;
    while (start < part.length() && (quoted || !is_number_character(part[start]))) {
        if (part[start] == '\'')
            quoted = !quoted;
        ++start;
    }
    size_t end = start;
    while (end < part.length() && is_number_character(part[end]))
        ++end;
    prefix = part.substring_view(0, start);
    number = part.substring_view(start, end - start);
    suffix = part.substring_view(end);
}

NumberPattern parse_pattern(StringView pattern)
{
    NumberPattern result;
    auto const separator = pattern.find(';');
    auto const positive = separator.has_value() ? pattern.substring_view(0, separator.value()) : pattern;
    StringView number;
    split_part(positive, result.positive_prefix, number, result.positive_suffix);
    if (separator.has_value()) {
        StringView ignored;
        split_part(pattern.substring_view(separator.value() + 1), result.negative_prefix, ignored, result.negative_suffix);
        result.has_negative = true;
    }
    auto const point = number.find('.');
    auto const integer = point.has_value() ? number.substring_view(0, point.value()) : number;
    result.minimum_integer_digits = 0;
    int digits_since_comma = 0;
    int previous_group = 0;
    bool any_comma = false;
    for (size_t i = 0; i < integer.length(); ++i) {
        char const c = integer[i];
        if (c == ',') {
            if (any_comma)
                previous_group = digits_since_comma;
            any_comma = true;
            digits_since_comma = 0;
        } else {
            ++digits_since_comma;
            if (c == '0')
                ++result.minimum_integer_digits;
        }
    }
    if (any_comma) {
        result.primary_group = digits_since_comma;
        result.secondary_group = previous_group ? previous_group : digits_since_comma;
    }
    if (point.has_value()) {
        auto const fraction = number.substring_view(point.value() + 1);
        for (size_t i = 0; i < fraction.length(); ++i) {
            ++result.maximum_fraction_digits;
            if (fraction[i] == '0')
                ++result.minimum_fraction_digits;
        }
    }
    return result;
}

// A prefix or suffix, with its symbols in. Quotes are for what would otherwise be taken for one.
void append_affix(StringBuilder& out, StringView affix, LocaleData const& locale, StringView currency_symbol)
{
    bool quoted = false;
    for (size_t i = 0; i < affix.length(); ++i) {
        char const c = affix[i];
        if (c == '\'') {
            quoted = !quoted;
            continue;
        }
        if (quoted) {
            out.append(c);
            continue;
        }
        if (c == '%') {
            out.append(StringView(locale.percent_sign));
        } else if (c == '-') {
            out.append(StringView(locale.minus_sign));
        } else if (c == '+') {
            out.append(StringView(locale.plus_sign));
        } else if (static_cast<u8>(c) == 0xc2 && i + 1 < affix.length() && static_cast<u8>(affix[i + 1]) == 0xa4) { // ¤
            out.append(currency_symbol);
            ++i;
        } else {
            out.append(c);
        }
    }
}

// The digit in the locale's own way of writing them.
void append_digit(StringBuilder& out, LocaleData const& locale, int digit)
{
    StringView digits { locale.digits };
    size_t at = 0;
    for (int i = 0; i < digit; ++i)
        at += Unicode::decode_utf8(digits.substring_view(at)).length;
    auto const decoded = Unicode::decode_utf8(digits.substring_view(at));
    out.append(digits.substring_view(at, decoded.length));
}

ErrorOr<String> format_with(LocaleData const& locale, StringView pattern_text, double value, StringView currency_symbol, int fraction_override = -1)
{
    auto const pattern = parse_pattern(pattern_text);
    bool const negative = __builtin_signbit(value) && value == value;
    double const magnitude = negative ? -value : value;

    StringBuilder out;
    // The prefix.
    if (negative) {
        if (pattern.has_negative) {
            append_affix(out, pattern.negative_prefix, locale, currency_symbol);
        } else {
            out.append(StringView(locale.minus_sign));
            append_affix(out, pattern.positive_prefix, locale, currency_symbol);
        }
    } else {
        append_affix(out, pattern.positive_prefix, locale, currency_symbol);
    }

    if (value != value) {
        out.append(StringView(locale.not_a_number));
    } else if (magnitude > 1.7976931348623157e308) {
        out.append(StringView(locale.infinity));
    } else {
        int const most_fraction = fraction_override >= 0 ? fraction_override : pattern.maximum_fraction_digits;
        int const least_fraction = fraction_override >= 0 ? fraction_override : pattern.minimum_fraction_digits;
        char text[512];
        __builtin_snprintf(text, sizeof(text), "%.*f", most_fraction, magnitude);
        StringView const printed { text };
        auto const point = printed.find('.');
        auto const integer = point.has_value() ? printed.substring_view(0, point.value()) : printed;
        auto fraction = point.has_value() ? printed.substring_view(point.value() + 1) : StringView {};
        while (fraction.length() > static_cast<size_t>(least_fraction) && fraction[fraction.length() - 1] == '0')
            fraction = fraction.substring_view(0, fraction.length() - 1);

        // A currency symbol that's letters doesn't go right up against a number, if there's nothing else between them.
        if (!currency_symbol.is_empty() && !pattern.has_negative) {
            auto const prefix = pattern.positive_prefix;
            bool const symbol_last = prefix.ends_with("\xc2\xa4"_sv);
            if (symbol_last) {
                Unicode::CodePoint last = 0;
                StringView remaining = currency_symbol;
                while (!remaining.is_empty()) {
                    auto const decoded = Unicode::decode_utf8(remaining);
                    last = decoded.code_point;
                    remaining = remaining.substring_view(decoded.length);
                }
                if (!Unicode::is_symbol(last) && Unicode::general_category(last) != Unicode::GeneralCategory::Zs)
                    out.append(StringView(locale.currency_spacing));
            }
        }

        // Digits, grouped from the right: first by the primary size, then the secondary, unless there aren't enough to be worth it.
        int const digit_count = static_cast<int>(integer.length());
        bool const grouping = pattern.primary_group > 0 && digit_count >= pattern.primary_group + locale.minimum_grouping_digits;
        for (int i = 0; i < digit_count; ++i) {
            append_digit(out, locale, integer[static_cast<size_t>(i)] - '0');
            int const remaining = digit_count - 1 - i;
            if (grouping && remaining > 0) {
                bool group_here = remaining == pattern.primary_group;
                if (remaining > pattern.primary_group && pattern.secondary_group > 0)
                    group_here = (remaining - pattern.primary_group) % pattern.secondary_group == 0;
                if (group_here)
                    out.append(StringView(locale.group_separator));
            }
        }
        if (!fraction.is_empty()) {
            out.append(StringView(locale.decimal_separator));
            for (size_t i = 0; i < fraction.length(); ++i)
                append_digit(out, locale, fraction[i] - '0');
        }

        if (!currency_symbol.is_empty() && pattern.positive_suffix.starts_with("\xc2\xa4"_sv)) {
            auto const first = Unicode::decode_utf8(currency_symbol).code_point;
            if (!Unicode::is_symbol(first) && Unicode::general_category(first) != Unicode::GeneralCategory::Zs)
                out.append(StringView(locale.currency_spacing));
        }
    }

    // The suffix.
    if (negative && pattern.has_negative)
        append_affix(out, pattern.negative_suffix, locale, currency_symbol);
    else
        append_affix(out, pattern.positive_suffix, locale, currency_symbol);
    return out.to_string();
}

}

ErrorOr<String> format_number(LocaleData const& locale, double value)
{
    return format_with(locale, StringView(locale.decimal_pattern), value, {});
}

ErrorOr<String> format_percent(LocaleData const& locale, double value)
{
    return format_with(locale, StringView(locale.percent_pattern), value * 100, {});
}

ErrorOr<String> format_currency(LocaleData const& locale, double value, StringView code)
{
    if (code.is_empty())
        code = StringView(locale.default_currency);
    for (size_t i = 0; i < currency_count; ++i) {
        if (StringView(currency_codes[i]) == code)
            return format_with(locale, StringView(locale.currency_pattern), value, StringView(locale.currency_symbols[i]), currency_decimal_places[i]);
    }
    // One with nothing known about it goes by its code, with cents.
    return format_with(locale, StringView(locale.currency_pattern), value, code, 2);
}

}
