/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUnicode/Tables.h>
#include <LibUnicode/Unicode.h>

// Where text divides into what a person would call characters, words and sentences: UAX #29's rules, in its order, with its numbers.

namespace Unicode {

using namespace Tables;

GraphemeBreak grapheme_break_of(CodePoint code)
{
    return static_cast<GraphemeBreak>(run_value(Tables::grapheme_break, grapheme_break_count, code));
}

WordBreak word_break_of(CodePoint code)
{
    return static_cast<WordBreak>(run_value(Tables::word_break, word_break_count, code));
}

SentenceBreak sentence_break_of(CodePoint code)
{
    return static_cast<SentenceBreak>(run_value(Tables::sentence_break, sentence_break_count, code));
}

namespace {

IndicConjunctBreak conjunct_break_of(CodePoint code)
{
    return static_cast<IndicConjunctBreak>(run_value(Tables::indic_conjunct_break, indic_conjunct_break_count, code));
}

// Is there a boundary before `index`?
bool is_grapheme_boundary(Span<CodePoint const> text, size_t index)
{
    using GB = GraphemeBreak;
    // GB1, GB2
    if (index == 0 || index == text.size())
        return true;
    auto const before = grapheme_break_of(text[index - 1]);
    auto const after = grapheme_break_of(text[index]);
    // GB3
    if (before == GB::CR && after == GB::LF)
        return false;
    // GB4
    if (before == GB::Control || before == GB::CR || before == GB::LF)
        return true;
    // GB5
    if (after == GB::Control || after == GB::CR || after == GB::LF)
        return true;
    // GB6
    if (before == GB::L && (after == GB::L || after == GB::V || after == GB::LV || after == GB::LVT))
        return false;
    // GB7
    if ((before == GB::LV || before == GB::V) && (after == GB::V || after == GB::T))
        return false;
    // GB8
    if ((before == GB::LVT || before == GB::T) && after == GB::T)
        return false;
    // GB9
    if (after == GB::Extend || after == GB::ZWJ)
        return false;
    // GB9a
    if (after == GB::SpacingMark)
        return false;
    // GB9b
    if (before == GB::Prepend)
        return false;
    // GB9c: a consonant, then a linker with whatever else goes with one, and another consonant.
    if (conjunct_break_of(text[index]) == IndicConjunctBreak::Consonant) {
        bool linked = false;
        for (size_t i = index; i > 0; --i) {
            auto const conjunct = conjunct_break_of(text[i - 1]);
            if (conjunct == IndicConjunctBreak::Linker) {
                linked = true;
            } else if (conjunct == IndicConjunctBreak::Consonant) {
                if (linked)
                    return false;
                break;
            } else if (conjunct != IndicConjunctBreak::Extend) {
                break;
            }
        }
    }
    // GB11: emoji joined up.
    if (before == GB::ZWJ && Unicode::is_extended_pictographic(text[index])) {
        size_t i = index - 1;
        while (i > 0 && grapheme_break_of(text[i - 1]) == GB::Extend)
            --i;
        if (i > 0 && Unicode::is_extended_pictographic(text[i - 1]))
            return false;
    }
    // GB12, GB13: flags are in pairs, however many there are in a row.
    if (before == GB::Regional_Indicator && after == GB::Regional_Indicator) {
        size_t count = 0;
        for (size_t i = index; i > 0 && grapheme_break_of(text[i - 1]) == GB::Regional_Indicator; --i)
            ++count;
        if (count % 2 == 1)
            return false;
    }
    // GB999
    return true;
}

bool is_word_boundary(Span<CodePoint const> text, size_t index)
{
    using WB = WordBreak;
    // WB1, WB2
    if (index == 0 || index == text.size())
        return true;
    auto const before_raw = word_break_of(text[index - 1]);
    auto const after = word_break_of(text[index]);
    // WB3
    if (before_raw == WB::CR && after == WB::LF)
        return false;
    // WB3a
    if (before_raw == WB::Newline || before_raw == WB::CR || before_raw == WB::LF)
        return true;
    // WB3b
    if (after == WB::Newline || after == WB::CR || after == WB::LF)
        return true;
    // WB3c
    if (before_raw == WB::ZWJ && Unicode::is_extended_pictographic(text[index]))
        return false;
    // WB3d
    if (before_raw == WB::WSegSpace && after == WB::WSegSpace)
        return false;
    // WB4: what doesn't count, unless it's where something's starting.
    if (after == WB::Extend || after == WB::Format || after == WB::ZWJ)
        return false;

    auto const skips = [](WB value) { return value == WB::Extend || value == WB::Format || value == WB::ZWJ; };
    auto const before_at = [&](size_t& i) -> WB {
        // What's before `i`, looking through what doesn't count; `i` then says where that was. Other, at the start.
        while (i > 0) {
            --i;
            auto const value = word_break_of(text[i]);
            if (!skips(value) || i == 0)
                return skips(value) ? WB::Other : value;
            auto const previous = word_break_of(text[i - 1]);
            // These stop it: WB4 doesn't go through them.
            if (previous == WB::Newline || previous == WB::CR || previous == WB::LF)
                return value;
        }
        return WB::Other;
    };
    auto const after_at = [&](size_t i) -> WB {
        // What's after `i`, the same way.
        for (++i; i < text.size(); ++i) {
            auto const value = word_break_of(text[i]);
            if (!skips(value))
                return value;
        }
        return WB::Other;
    };
    auto const is_ah_letter = [](WB value) { return value == WB::ALetter || value == WB::Hebrew_Letter; };
    auto const is_mid_num_let_q = [](WB value) { return value == WB::MidNumLet || value == WB::Single_Quote; };

    size_t cursor = index;
    auto const before = before_at(cursor);
    size_t cursor_2 = cursor;
    auto const before_2 = cursor_2 > 0 ? before_at(cursor_2) : WB::Other;
    auto const after_2 = after_at(index);

    // WB5
    if (is_ah_letter(before) && is_ah_letter(after))
        return false;
    // WB6
    if (is_ah_letter(before) && (after == WB::MidLetter || is_mid_num_let_q(after)) && is_ah_letter(after_2))
        return false;
    // WB7
    if (is_ah_letter(before_2) && (before == WB::MidLetter || is_mid_num_let_q(before)) && is_ah_letter(after))
        return false;
    // WB7a
    if (before == WB::Hebrew_Letter && after == WB::Single_Quote)
        return false;
    // WB7b
    if (before == WB::Hebrew_Letter && after == WB::Double_Quote && after_2 == WB::Hebrew_Letter)
        return false;
    // WB7c
    if (before_2 == WB::Hebrew_Letter && before == WB::Double_Quote && after == WB::Hebrew_Letter)
        return false;
    // WB8
    if (before == WB::Numeric && after == WB::Numeric)
        return false;
    // WB9
    if (is_ah_letter(before) && after == WB::Numeric)
        return false;
    // WB10
    if (before == WB::Numeric && is_ah_letter(after))
        return false;
    // WB11
    if (before_2 == WB::Numeric && (before == WB::MidNum || is_mid_num_let_q(before)) && after == WB::Numeric)
        return false;
    // WB12
    if (before == WB::Numeric && (after == WB::MidNum || is_mid_num_let_q(after)) && after_2 == WB::Numeric)
        return false;
    // WB13
    if (before == WB::Katakana && after == WB::Katakana)
        return false;
    // WB13a
    if ((is_ah_letter(before) || before == WB::Numeric || before == WB::Katakana || before == WB::ExtendNumLet) && after == WB::ExtendNumLet)
        return false;
    // WB13b
    if (before == WB::ExtendNumLet && (is_ah_letter(after) || after == WB::Numeric || after == WB::Katakana))
        return false;
    // WB15, WB16
    if (before == WB::Regional_Indicator && after == WB::Regional_Indicator) {
        size_t count = 0;
        size_t i = index;
        while (i > 0) {
            auto const value = before_at(i);
            if (value != WB::Regional_Indicator)
                break;
            ++count;
        }
        if (count % 2 == 1)
            return false;
    }
    // WB999
    return true;
}

bool is_sentence_boundary(Span<CodePoint const> text, size_t index)
{
    using SB = SentenceBreak;
    // SB1, SB2
    if (index == 0 || index == text.size())
        return true;
    auto const before_raw = sentence_break_of(text[index - 1]);
    auto const after = sentence_break_of(text[index]);
    auto const is_para_sep = [](SB value) { return value == SB::Sep || value == SB::CR || value == SB::LF; };
    // SB3
    if (before_raw == SB::CR && after == SB::LF)
        return false;
    // SB4
    if (is_para_sep(before_raw))
        return true;
    // SB5
    if (after == SB::Extend || after == SB::Format)
        return false;

    // Everything from here on looks through those two. In this list, the ones that count, going back from here.
    auto const previous_counting = [&](size_t& i) -> SB {
        while (i > 0) {
            --i;
            auto const value = sentence_break_of(text[i]);
            // They go with what's before them, unless that's the end of a paragraph or there's nothing there. Then they're on their own (SB5).
            if ((value == SB::Extend || value == SB::Format) && i > 0 && !is_para_sep(sentence_break_of(text[i - 1])))
                continue;
            return value;
        }
        return SB::Other;
    };
    auto const next_counting = [&](size_t i) -> SB {
        for (; i < text.size(); ++i) {
            auto const value = sentence_break_of(text[i]);
            if (value != SB::Extend && value != SB::Format)
                return value;
        }
        return SB::Other;
    };
    auto const is_sa_term = [](SB value) { return value == SB::STerm || value == SB::ATerm; };

    size_t i = index;
    auto const before = previous_counting(i);
    size_t after_before = i;
    auto const before_2 = after_before > 0 ? previous_counting(after_before) : SB::Other;

    // SB6
    if (before == SB::ATerm && after == SB::Numeric)
        return false;
    // SB7
    if (before == SB::ATerm && (before_2 == SB::Upper || before_2 == SB::Lower) && after == SB::Upper)
        return false;

    // What the rest are about: a full stop, then any closers, then any spaces, and perhaps the end of a paragraph, which is where we are.
    size_t cursor = index;
    auto current = previous_counting(cursor);
    bool saw_para_sep = false;
    bool saw_space = false;
    bool saw_close = false;
    if (is_para_sep(current)) {
        saw_para_sep = true;
        current = cursor > 0 ? previous_counting(cursor) : SB::Other;
    }
    while (current == SB::Sp) {
        saw_space = true;
        if (cursor == 0) {
            current = SB::Other;
            break;
        }
        current = previous_counting(cursor);
    }
    while (current == SB::Close) {
        saw_close = true;
        if (cursor == 0) {
            current = SB::Other;
            break;
        }
        current = previous_counting(cursor);
    }
    bool const after_term = is_sa_term(current);
    (void)saw_close;
    if (after_term && !saw_para_sep) {
        // SB8: lowercase coming up means that wasn't the end of it.
        if (current == SB::ATerm) {
            for (size_t n = index; n < text.size(); ++n) {
                auto const value = sentence_break_of(text[n]);
                if (value == SB::Lower)
                    return false;
                if (value == SB::OLetter || value == SB::Upper || is_para_sep(value) || is_sa_term(value))
                    break;
            }
        }
        // SB8a
        if (after == SB::SContinue || is_sa_term(after))
            return false;
        // SB9
        if (!saw_space && (after == SB::Close || after == SB::Sp || is_para_sep(after)))
            return false;
        // SB10
        if (after == SB::Sp || is_para_sep(after))
            return false;
    }
    (void)next_counting;
    // SB11
    if (after_term)
        return true;
    // SB998
    return false;
}

template<typename Check>
ErrorOr<Vector<size_t>> boundaries_by(Span<CodePoint const> text, Check is_boundary)
{
    Vector<size_t> result;
    TRY(result.try_append(0));
    for (size_t i = 1; i <= text.size(); ++i) {
        if (is_boundary(text, i))
            TRY(result.try_append(i));
    }
    return result;
}

}

ErrorOr<Vector<size_t>> grapheme_boundaries(Span<CodePoint const> text)
{
    return boundaries_by(text, is_grapheme_boundary);
}

ErrorOr<Vector<size_t>> word_boundaries(Span<CodePoint const> text)
{
    return boundaries_by(text, is_word_boundary);
}

ErrorOr<Vector<size_t>> sentence_boundaries(Span<CodePoint const> text)
{
    return boundaries_by(text, is_sentence_boundary);
}

size_t next_grapheme_boundary(Span<CodePoint const> text, size_t from)
{
    for (size_t i = from + 1; i <= text.size(); ++i) {
        if (is_grapheme_boundary(text, i))
            return i;
    }
    return text.size();
}

}
