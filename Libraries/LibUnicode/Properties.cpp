/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Tables.h>
#include <LibUnicode/Unicode.h>

namespace Unicode {

using namespace Tables;

GeneralCategory general_category(CodePoint code)
{
    return static_cast<GeneralCategory>(run_value(Tables::general_category, general_category_count, code));
}

Script script_of(CodePoint code)
{
    return static_cast<Script>(run_value(Tables::script, script_count, code));
}

StringView script_name(Script which)
{
    return StringView(script_names[to_underlying(which)]);
}

EastAsianWidth east_asian_width(CodePoint code)
{
    return static_cast<EastAsianWidth>(run_value(Tables::east_asian_width, east_asian_width_count, code));
}

int column_width(CodePoint code)
{
    if (code == 0)
        return 0;
    auto const category = Unicode::general_category(code);
    if (category == GeneralCategory::Cc || category == GeneralCategory::Cs || category == GeneralCategory::Cn)
        return -1;
    // What sits on the one before it, or isn't there to see.
    if (category == GeneralCategory::Mn || category == GeneralCategory::Me || category == GeneralCategory::Cf || Unicode::is_default_ignorable(code))
        return 0;
    // Conjoining vowels and final consonants, which go with what came first.
    if (code >= 0x1160 && code <= 0x11ff)
        return 0;
    auto const width = Unicode::east_asian_width(code);
    return width == EastAsianWidth::W || width == EastAsianWidth::F ? 2 : 1;
}

JoiningType joining_type_of(CodePoint code)
{
    return static_cast<JoiningType>(run_value(Tables::joining_type, joining_type_count, code));
}

JoiningGroup joining_group_of(CodePoint code)
{
    return static_cast<JoiningGroup>(run_value(Tables::joining_group, joining_group_count, code));
}

IndicSyllabicCategory indic_syllabic_category_of(CodePoint code)
{
    return static_cast<IndicSyllabicCategory>(run_value(Tables::indic_syllabic_category, indic_syllabic_category_count, code));
}

IndicPositionalCategory indic_positional_category_of(CodePoint code)
{
    return static_cast<IndicPositionalCategory>(run_value(Tables::indic_positional_category, indic_positional_category_count, code));
}

VerticalOrientation vertical_orientation_of(CodePoint code)
{
    return static_cast<VerticalOrientation>(run_value(Tables::vertical_orientation, vertical_orientation_count, code));
}

int decimal_digit_value(CodePoint code)
{
    if (auto const* entry = find_entry(decimal_digit, decimal_digit_count, 2, code))
        return static_cast<int>(entry[1]);
    return -1;
}

bool is_letter(CodePoint code)
{
    return to_underlying(Unicode::general_category(code)) <= to_underlying(GeneralCategory::Lo);
}

bool is_mark(CodePoint code)
{
    auto const category = Unicode::general_category(code);
    return category == GeneralCategory::Mn || category == GeneralCategory::Mc || category == GeneralCategory::Me;
}

bool is_number(CodePoint code)
{
    auto const category = Unicode::general_category(code);
    return category == GeneralCategory::Nd || category == GeneralCategory::Nl || category == GeneralCategory::No;
}

bool is_punctuation(CodePoint code)
{
    auto const category = to_underlying(Unicode::general_category(code));
    return category >= to_underlying(GeneralCategory::Pc) && category <= to_underlying(GeneralCategory::Po);
}

bool is_symbol(CodePoint code)
{
    auto const category = to_underlying(Unicode::general_category(code));
    return category >= to_underlying(GeneralCategory::Sm) && category <= to_underlying(GeneralCategory::So);
}

bool is_control(CodePoint code)
{
    return Unicode::general_category(code) == GeneralCategory::Cc;
}

bool is_assigned(CodePoint code)
{
    return Unicode::general_category(code) != GeneralCategory::Cn;
}

#define BOOLEAN_PROPERTY(name)                                                \
    bool name(CodePoint code)                                                 \
    {                                                                         \
        return has_property(Tables::name, Tables::name##_count, code);        \
    }

BOOLEAN_PROPERTY(is_alphabetic)
BOOLEAN_PROPERTY(is_uppercase)
BOOLEAN_PROPERTY(is_lowercase)
BOOLEAN_PROPERTY(is_cased)
BOOLEAN_PROPERTY(is_case_ignorable)
BOOLEAN_PROPERTY(is_white_space)
BOOLEAN_PROPERTY(is_id_start)
BOOLEAN_PROPERTY(is_id_continue)
BOOLEAN_PROPERTY(is_xid_start)
BOOLEAN_PROPERTY(is_xid_continue)
BOOLEAN_PROPERTY(is_math)
BOOLEAN_PROPERTY(is_default_ignorable)
BOOLEAN_PROPERTY(is_grapheme_extend)
BOOLEAN_PROPERTY(is_dash)
BOOLEAN_PROPERTY(is_quotation_mark)
BOOLEAN_PROPERTY(is_terminal_punctuation)
BOOLEAN_PROPERTY(is_diacritic)
BOOLEAN_PROPERTY(is_hex_digit)
BOOLEAN_PROPERTY(is_ideographic)
BOOLEAN_PROPERTY(is_unified_ideograph)
BOOLEAN_PROPERTY(is_noncharacter)
BOOLEAN_PROPERTY(is_variation_selector)
BOOLEAN_PROPERTY(is_regional_indicator)
BOOLEAN_PROPERTY(is_emoji)
BOOLEAN_PROPERTY(is_emoji_presentation)
BOOLEAN_PROPERTY(is_emoji_modifier)
BOOLEAN_PROPERTY(is_emoji_modifier_base)
BOOLEAN_PROPERTY(is_extended_pictographic)

#undef BOOLEAN_PROPERTY

u8 combining_class(CodePoint code)
{
    // Almost everything: and worth not looking for, since this is called for every character that's normalized.
    if (code < 0x300)
        return 0;
    return static_cast<u8>(combining_class_values[run_value(Tables::combining_class, combining_class_count, code)]);
}

}
