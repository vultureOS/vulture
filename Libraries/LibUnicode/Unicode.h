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
#include <LibUnicode/Generated/Enums.h>

// What Unicode says about text, from its own tables (Generated/, made by Meta/generate-unicode.py). All of it is checked against the files Unicode publishes for
// the purpose (Tests/LibUnicode).

namespace Unicode {

using CodePoint = u32;

constexpr CodePoint replacement_character = 0xfffd;
constexpr CodePoint last_code_point = 0x10ffff;

// ---- UTF-8.

struct Decoded {
    CodePoint code_point { replacement_character };
    size_t length { 1 };
};
// The character at the start of `text`, which mustn't be empty. What isn't valid comes to U+FFFD, for as much of it as could have been the start of something (as the
// Unicode Standard recommends, and as a browser does it).
[[nodiscard]] Decoded decode_utf8(StringView text);
[[nodiscard]] bool is_valid_utf8(StringView);
// How many bytes it took: 1 to 4, or 0 if it isn't a character there can be.
size_t encode_utf8(CodePoint, char out[4]);
[[nodiscard]] ErrorOr<Vector<CodePoint>> code_points_of(StringView utf8);
[[nodiscard]] ErrorOr<String> utf8_of(Span<CodePoint const>);

// ---- What each one is.

[[nodiscard]] GeneralCategory general_category(CodePoint);
[[nodiscard]] Script script_of(CodePoint);
[[nodiscard]] StringView script_name(Script);
[[nodiscard]] EastAsianWidth east_asian_width(CodePoint);
// How many columns on a terminal: 0, 1 or 2; -1 for what isn't for showing.
[[nodiscard]] int column_width(CodePoint);

// For shaping.
[[nodiscard]] JoiningType joining_type_of(CodePoint);
[[nodiscard]] JoiningGroup joining_group_of(CodePoint);
[[nodiscard]] IndicSyllabicCategory indic_syllabic_category_of(CodePoint);
[[nodiscard]] IndicPositionalCategory indic_positional_category_of(CodePoint);
[[nodiscard]] VerticalOrientation vertical_orientation_of(CodePoint);
// 0 to 9, or -1.
[[nodiscard]] int decimal_digit_value(CodePoint);
[[nodiscard]] bool is_letter(CodePoint);
[[nodiscard]] bool is_mark(CodePoint);
[[nodiscard]] bool is_number(CodePoint);
[[nodiscard]] bool is_punctuation(CodePoint);
[[nodiscard]] bool is_symbol(CodePoint);
[[nodiscard]] bool is_control(CodePoint);
[[nodiscard]] bool is_assigned(CodePoint);

[[nodiscard]] bool is_alphabetic(CodePoint);
[[nodiscard]] bool is_uppercase(CodePoint);
[[nodiscard]] bool is_lowercase(CodePoint);
[[nodiscard]] bool is_cased(CodePoint);
[[nodiscard]] bool is_case_ignorable(CodePoint);
[[nodiscard]] bool is_white_space(CodePoint);
[[nodiscard]] bool is_id_start(CodePoint);
[[nodiscard]] bool is_id_continue(CodePoint);
[[nodiscard]] bool is_xid_start(CodePoint);
[[nodiscard]] bool is_xid_continue(CodePoint);
[[nodiscard]] bool is_math(CodePoint);
[[nodiscard]] bool is_default_ignorable(CodePoint);
[[nodiscard]] bool is_grapheme_extend(CodePoint);
[[nodiscard]] bool is_dash(CodePoint);
[[nodiscard]] bool is_quotation_mark(CodePoint);
[[nodiscard]] bool is_terminal_punctuation(CodePoint);
[[nodiscard]] bool is_diacritic(CodePoint);
[[nodiscard]] bool is_hex_digit(CodePoint);
[[nodiscard]] bool is_ideographic(CodePoint);
[[nodiscard]] bool is_unified_ideograph(CodePoint);
[[nodiscard]] bool is_noncharacter(CodePoint);
[[nodiscard]] bool is_variation_selector(CodePoint);
[[nodiscard]] bool is_regional_indicator(CodePoint);
[[nodiscard]] bool is_emoji(CodePoint);
[[nodiscard]] bool is_emoji_presentation(CodePoint);
[[nodiscard]] bool is_emoji_modifier(CodePoint);
[[nodiscard]] bool is_emoji_modifier_base(CodePoint);
[[nodiscard]] bool is_extended_pictographic(CodePoint);

// ---- Case.

// One for one, which isn't always right: a German ß has no capital of its own to be.
[[nodiscard]] CodePoint simple_to_upper(CodePoint);
[[nodiscard]] CodePoint simple_to_lower(CodePoint);
[[nodiscard]] CodePoint simple_to_title(CodePoint);
[[nodiscard]] CodePoint simple_fold(CodePoint);

// However many it comes to: at most three. `locale` matters for Turkish, Azeri and Lithuanian; for anything else it can be empty.
[[nodiscard]] ErrorOr<Vector<CodePoint>> to_uppercase(Span<CodePoint const>, StringView locale = {});
[[nodiscard]] ErrorOr<Vector<CodePoint>> to_lowercase(Span<CodePoint const>, StringView locale = {});
[[nodiscard]] ErrorOr<Vector<CodePoint>> to_titlecase(Span<CodePoint const>, StringView locale = {});
// For comparing, and for nothing else: what two strings that only differ in case both come to.
[[nodiscard]] ErrorOr<Vector<CodePoint>> case_fold(Span<CodePoint const>, bool turkic = false);
[[nodiscard]] ErrorOr<String> to_uppercase(StringView utf8, StringView locale = {});
[[nodiscard]] ErrorOr<String> to_lowercase(StringView utf8, StringView locale = {});
[[nodiscard]] ErrorOr<String> case_fold(StringView utf8);

// ---- Normalization (UAX #15).

enum class NormalizationForm : u8 {
    NFD,
    NFC,
    NFKD,
    NFKC,
};

[[nodiscard]] u8 combining_class(CodePoint);
[[nodiscard]] ErrorOr<Vector<CodePoint>> normalize(Span<CodePoint const>, NormalizationForm);
[[nodiscard]] ErrorOr<String> normalize(StringView utf8, NormalizationForm);
[[nodiscard]] ErrorOr<bool> is_normalized(StringView utf8, NormalizationForm);
// One step of what normalizing does, for what has to choose (a shaper, by what a font has): 0 if these two aren't one character.
[[nodiscard]] CodePoint compose_pair(CodePoint first, CodePoint second);
[[nodiscard]] ErrorOr<void> append_canonical_decomposition(Vector<CodePoint>&, CodePoint);

// ---- Where text can be divided (UAX #29): each is every boundary there is, in order, from 0 to the length.

[[nodiscard]] ErrorOr<Vector<size_t>> grapheme_boundaries(Span<CodePoint const>);
[[nodiscard]] ErrorOr<Vector<size_t>> word_boundaries(Span<CodePoint const>);
[[nodiscard]] ErrorOr<Vector<size_t>> sentence_boundaries(Span<CodePoint const>);
[[nodiscard]] GraphemeBreak grapheme_break_of(CodePoint);
[[nodiscard]] WordBreak word_break_of(CodePoint);
[[nodiscard]] SentenceBreak sentence_break_of(CodePoint);
// For moving through text a character at a time, with no list to make: what comes after `from`, in what a person would call characters.
[[nodiscard]] size_t next_grapheme_boundary(Span<CodePoint const>, size_t from);

// ---- Where a line can end (UAX #14).

struct LineBreakOpportunity {
    size_t position { 0 };
    bool mandatory { false };
};
// In order, and the end of the text is always the last.
[[nodiscard]] ErrorOr<Vector<LineBreakOpportunity>> line_break_opportunities(Span<CodePoint const>);
[[nodiscard]] LineBreakClass line_break_class_of(CodePoint);

// ---- Which way it reads (UAX #9).

enum class ParagraphDirection : u8 {
    Auto,
    LeftToRight,
    RightToLeft,
};

// What's taken out by the algorithm has no level of its own: embedding controls and the like.
constexpr u8 bidi_level_removed = 0xff;

struct BidiParagraph {
    u8 paragraph_level { 0 };
    // One for each code point, of the whole paragraph, with what's only known once it's been divided into lines still to do: reordering_levels() does that.
    Vector<u8> levels;
    Vector<BidiClass> classes;
};

[[nodiscard]] BidiClass bidi_class_of(CodePoint);
[[nodiscard]] CodePoint bidi_mirrored(CodePoint);
// One paragraph: any paragraph separator should be the last of it.
[[nodiscard]] ErrorOr<BidiParagraph> resolve_bidi(Span<CodePoint const>, ParagraphDirection = ParagraphDirection::Auto);
// With no characters, only classes: there can't be any brackets, then. This is what Unicode's test for the algorithm wants.
[[nodiscard]] ErrorOr<BidiParagraph> resolve_bidi(Span<BidiClass const>, ParagraphDirection = ParagraphDirection::Auto);
// The levels for what's from `start` up to `end` as a line: rule L1.
[[nodiscard]] ErrorOr<Vector<u8>> reordering_levels(BidiParagraph const&, size_t start, size_t end);
// Rule L2, of a line's levels: which goes where, from the left. What's been removed isn't in it.
[[nodiscard]] ErrorOr<Vector<size_t>> visual_order(Span<u8 const> levels);

}
