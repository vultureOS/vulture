/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// The tables Meta/generate-unicode.py makes, and how to look things up in them.

namespace Unicode::Tables {

#define UNICODE_TABLE(name)   \
    extern u32 const name[];  \
    extern size_t const name##_count;

UNICODE_TABLE(general_category)
UNICODE_TABLE(combining_class)
UNICODE_TABLE(combining_class_values)
UNICODE_TABLE(script)
UNICODE_TABLE(east_asian_width)
UNICODE_TABLE(simple_upper)
UNICODE_TABLE(simple_lower)
UNICODE_TABLE(simple_title)
UNICODE_TABLE(special_upper)
UNICODE_TABLE(special_lower)
UNICODE_TABLE(special_title)
UNICODE_TABLE(simple_fold)
UNICODE_TABLE(full_fold)
UNICODE_TABLE(turkic_fold)
UNICODE_TABLE(decimal_digit)
UNICODE_TABLE(decompositions)
UNICODE_TABLE(decomposition_sequences)
UNICODE_TABLE(compositions)
UNICODE_TABLE(grapheme_break)
UNICODE_TABLE(word_break)
UNICODE_TABLE(sentence_break)
UNICODE_TABLE(indic_conjunct_break)
UNICODE_TABLE(line_break)
UNICODE_TABLE(bidi_class)
UNICODE_TABLE(bidi_mirror)
UNICODE_TABLE(bidi_brackets)
UNICODE_TABLE(joining_type)
UNICODE_TABLE(joining_group)
UNICODE_TABLE(indic_syllabic_category)
UNICODE_TABLE(indic_positional_category)
UNICODE_TABLE(vertical_orientation)

UNICODE_TABLE(is_alphabetic)
UNICODE_TABLE(is_uppercase)
UNICODE_TABLE(is_lowercase)
UNICODE_TABLE(is_cased)
UNICODE_TABLE(is_case_ignorable)
UNICODE_TABLE(is_id_start)
UNICODE_TABLE(is_id_continue)
UNICODE_TABLE(is_xid_start)
UNICODE_TABLE(is_xid_continue)
UNICODE_TABLE(is_math)
UNICODE_TABLE(is_default_ignorable)
UNICODE_TABLE(is_grapheme_extend)
UNICODE_TABLE(is_grapheme_base)
UNICODE_TABLE(is_white_space)
UNICODE_TABLE(is_dash)
UNICODE_TABLE(is_hyphen_like)
UNICODE_TABLE(is_quotation_mark)
UNICODE_TABLE(is_terminal_punctuation)
UNICODE_TABLE(is_diacritic)
UNICODE_TABLE(is_extender)
UNICODE_TABLE(is_hex_digit)
UNICODE_TABLE(is_ideographic)
UNICODE_TABLE(is_unified_ideograph)
UNICODE_TABLE(is_join_control)
UNICODE_TABLE(is_noncharacter)
UNICODE_TABLE(is_soft_dotted)
UNICODE_TABLE(is_variation_selector)
UNICODE_TABLE(is_regional_indicator)
UNICODE_TABLE(is_emoji)
UNICODE_TABLE(is_emoji_presentation)
UNICODE_TABLE(is_emoji_modifier)
UNICODE_TABLE(is_emoji_modifier_base)
UNICODE_TABLE(is_emoji_component)
UNICODE_TABLE(is_extended_pictographic)

#undef UNICODE_TABLE

extern char const* const script_names[];

// A table of runs: each entry says where one starts, in its upper 24 bits, and what the value is from there on, in its lower 8.
inline u8 run_value(u32 const* table, size_t count, u32 code)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        if ((table[middle] >> 8) <= code)
            low = middle + 1;
        else
            high = middle;
    }
    return low ? static_cast<u8>(table[low - 1]) : 0;
}

// A table of where a yes-or-no property changes, starting from no.
inline bool has_property(u32 const* table, size_t count, u32 code)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        if (table[middle] <= code)
            low = middle + 1;
        else
            high = middle;
    }
    return low & 1;
}

// A table of entries of `stride` words, by their first: the entry for this code point, if there is one.
inline u32 const* find_entry(u32 const* table, size_t count, size_t stride, u32 code)
{
    size_t low = 0;
    size_t high = count / stride;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        u32 const key = table[middle * stride];
        if (key == code)
            return table + middle * stride;
        if (key < code)
            low = middle + 1;
        else
            high = middle;
    }
    return nullptr;
}

}
