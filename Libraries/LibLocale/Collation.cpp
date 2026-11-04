/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibLocale/CollationTables.h>
#include <LibLocale/Locale.h>

// How to sort words: UTS #10, with CLDR's root table and its tailorings for those languages that have something to say about it.

namespace Locale {

using Unicode::CodePoint;
using namespace Tables;

namespace {

u32 const* find_pair(u32 const* table, size_t count, u32 code)
{
    size_t low = 0;
    size_t high = count / 2;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        u32 const key = table[middle * 2];
        if (key == code)
            return table + middle * 2;
        if (key < code)
            low = middle + 1;
        else
            high = middle;
    }
    return nullptr;
}

// Contractions are by up to three code points.
u32 const* find_contraction(CodePoint a, CodePoint b, CodePoint c)
{
    size_t low = 0;
    size_t high = contraction_entries_count / 4;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        u32 const* entry = contraction_entries + middle * 4;
        if (entry[0] == a && entry[1] == b && entry[2] == c)
            return entry;
        bool const before = entry[0] < a || (entry[0] == a && (entry[1] < b || (entry[1] == b && entry[2] < c)));
        if (before)
            low = middle + 1;
        else
            high = middle;
    }
    return nullptr;
}

// Is it the start of any contraction at all? Almost nothing is.
bool starts_contraction(CodePoint a)
{
    size_t low = 0;
    size_t high = contraction_entries_count / 4;
    while (low < high) {
        size_t const middle = (low + high) / 2;
        if (contraction_entries[middle * 4] < a)
            low = middle + 1;
        else
            high = middle;
    }
    return low < contraction_entries_count / 4 && contraction_entries[low * 4] == a;
}

}

ErrorOr<void> Collator::elements_for_rule_text(Span<CodePoint const> text, Vector<Element>& out) const
{
    auto const normalized = TRY(Unicode::normalize(text, Unicode::NormalizationForm::NFD));
    return elements_of(normalized.span(), out);
}

Collator::Tailored const* Collator::longest_tailored_match(Span<CodePoint const> text, size_t at, size_t& length) const
{
    Tailored const* best = nullptr;
    length = 0;
    for (auto const& tailored : m_tailored) {
        size_t const size = tailored.key.size();
        if (size <= length || at + size > text.size())
            continue;
        bool matches = true;
        for (size_t i = 0; i < size && matches; ++i)
            matches = text[at + i] == tailored.key[i];
        if (matches) {
            best = &tailored;
            length = size;
        }
    }
    return best;
}

ErrorOr<void> Collator::elements_of(Span<CodePoint const> text, Vector<Element>& out) const
{
    auto const unpack = [](u32 packed) {
        return Element { (packed >> 16) << 8, static_cast<u16>((packed >> 6) & 0x3ff), static_cast<u8>((packed >> 1) & 0x1f), (packed & 1) != 0 };
    };
    auto const append_packed = [&](u32 reference) -> ErrorOr<void> {
        u32 const start = reference >> 8;
        u32 const count = reference & 0xff;
        for (u32 i = 0; i < count; ++i)
            TRY(out.try_append(unpack(collation_elements[start + i])));
        return {};
    };

    // Non-starters that have gone with what's before them aren't to be looked at again.
    Vector<bool> taken;
    TRY(taken.try_resize(text.size()));
    for (size_t i = 0; i < text.size(); ++i)
        taken[i] = false;

    for (size_t i = 0; i < text.size();) {
        if (taken[i]) {
            ++i;
            continue;
        }
        CodePoint const first = text[i];

        if (!m_tailored.is_empty()) {
            size_t length = 0;
            if (auto const* tailored = longest_tailored_match(text, i, length)) {
                for (auto const& element : tailored->elements)
                    TRY(out.try_append(element));
                i += length;
                continue;
            }
        }

        // S2.1: the longest match there is, next to each other.
        if (starts_contraction(first)) {
            u32 const* best = nullptr;
            size_t used = 1;
            if (i + 2 < text.size() && !taken[i + 1] && !taken[i + 2]) {
                if (auto const* entry = find_contraction(first, text[i + 1], text[i + 2])) {
                    best = entry;
                    used = 3;
                }
            }
            if (!best && i + 1 < text.size() && !taken[i + 1]) {
                if (auto const* entry = find_contraction(first, text[i + 1], 0)) {
                    best = entry;
                    used = 2;
                }
            }
            // S2.1.1: and marks further on that could have been here, if nothing that would get in the way is between.
            CodePoint matched[3] = { first, used >= 2 ? text[i + 1] : 0, used >= 3 ? text[i + 2] : 0 };
            size_t matched_count = used;
            u8 last_class = 0;
            size_t const search_from = i + used;
            size_t scanned_last = search_from;
            for (size_t j = search_from; j < text.size() && matched_count < 3; ++j) {
                u8 const combining = Unicode::combining_class(text[j]);
                if (combining == 0)
                    break;
                bool const blocked = last_class != 0 && last_class >= combining;
                if (!blocked && !taken[j]) {
                    auto const* entry = matched_count == 1 ? find_contraction(matched[0], text[j], 0) : find_contraction(matched[0], matched[1], text[j]);
                    if (entry) {
                        best = entry;
                        matched[matched_count++] = text[j];
                        taken[j] = true;
                        continue;
                    }
                }
                last_class = combining;
                scanned_last = j;
            }
            (void)scanned_last;
            if (best) {
                TRY(append_packed(best[3]));
                i += used;
                continue;
            }
        }

        // S2.2: on its own.
        if (auto const* entry = find_pair(single_elements, single_elements_count, first)) {
            TRY(out.try_append(unpack(entry[1])));
        } else if (auto const* expansion = find_pair(expansions, expansions_count, first)) {
            TRY(append_packed(expansion[1]));
        } else {
            // S2.3: what isn't listed gets a place from what it is. These go in two elements, as there are more of them than there's room for.
            u32 base;
            u32 low;
            if ((first >= 0x17000 && first <= 0x18aff) || (first >= 0x18d00 && first <= 0x18d8f)) {
                base = 0xfb00;
                low = (first - 0x17000) | 0x8000;
            } else if (first >= 0x1b170 && first <= 0x1b2ff) {
                base = 0xfb01;
                low = (first - 0x1b170) | 0x8000;
            } else if (first >= 0x18b00 && first <= 0x18cff) {
                base = 0xfb02;
                low = (first - 0x18b00) | 0x8000;
            } else {
                bool const unified = Unicode::is_unified_ideograph(first);
                bool const core = (first >= 0x4e00 && first <= 0x9fff) || (first >= 0xf900 && first <= 0xfaff);
                base = (unified ? (core ? 0xfb40 : 0xfb80) : 0xfbc0) + (first >> 15);
                low = (first & 0x7fff) | 0x8000;
            }
            TRY(out.try_append(Element { base << 8, 0x20, 0x02, false }));
            TRY(out.try_append(Element { low << 8, 0, 0, false }));
        }
        ++i;
    }
    return {};
}

u32 Collator::reordered(u32 primary) const
{
    if (m_moved_first == 0 || primary < m_moved_to)
        return primary;
    if (primary >= m_moved_first && primary <= m_moved_last)
        return m_moved_to + (primary - m_moved_first);
    if (primary < m_moved_first)
        return primary + (m_moved_last - m_moved_first + 1);
    return primary;
}

ErrorOr<Vector<u32>> Collator::sort_key(Span<CodePoint const> text) const
{
    auto const normalized = TRY(Unicode::normalize(text, Unicode::NormalizationForm::NFD));
    Vector<Element> elements;
    TRY(elements.try_ensure_capacity(normalized.size()));
    TRY(elements_of(normalized.span(), elements));

    Vector<u32> key;
    TRY(key.try_ensure_capacity(elements.size() * 3));
    bool const shifted = m_variable == Variable::Shifted;

    // Whether each is one that's been shifted out, or is an ignorable that came after one.
    auto const for_each_weight = [&](auto weight_of) -> ErrorOr<void> {
        bool after_variable = false;
        for (auto const& element : elements) {
            bool const variable = shifted && element.variable;
            bool const ignored = after_variable && element.primary == 0;
            if (variable)
                after_variable = true;
            else if (element.primary != 0)
                after_variable = false;
            u32 const value = weight_of(element, variable, ignored);
            if (value)
                TRY(key.try_append(value));
        }
        return {};
    };

    TRY(for_each_weight([&](Element const& element, bool variable, bool) -> u32 { return variable ? 0 : reordered(element.primary); }));
    if (m_strength == Strength::Primary)
        return key;
    TRY(key.try_append(0));
    TRY(for_each_weight([](Element const& element, bool variable, bool ignored) -> u32 { return variable || ignored ? 0 : element.secondary; }));
    if (m_strength == Strength::Secondary)
        return key;
    TRY(key.try_append(0));
    TRY(for_each_weight([](Element const& element, bool variable, bool ignored) -> u32 { return variable || ignored ? 0 : element.tertiary; }));
    if (m_strength == Strength::Tertiary || !shifted)
        return key;
    TRY(key.try_append(0));
    TRY(for_each_weight([&](Element const& element, bool variable, bool ignored) -> u32 {
        if (variable)
            return reordered(element.primary);
        if (ignored || (element.primary == 0 && element.secondary == 0 && element.tertiary == 0))
            return 0;
        return 0xffffff;
    }));
    return key;
}

ErrorOr<int> Collator::compare(Span<CodePoint const> a, Span<CodePoint const> b) const
{
    auto const key_a = TRY(sort_key(a));
    auto const key_b = TRY(sort_key(b));
    size_t const shared = min(key_a.size(), key_b.size());
    for (size_t i = 0; i < shared; ++i) {
        if (key_a[i] != key_b[i])
            return key_a[i] < key_b[i] ? -1 : 1;
    }
    if (key_a.size() != key_b.size())
        return key_a.size() < key_b.size() ? -1 : 1;
    return 0;
}

ErrorOr<int> Collator::compare(StringView a, StringView b) const
{
    auto const a_points = TRY(Unicode::code_points_of(a));
    auto const b_points = TRY(Unicode::code_points_of(b));
    return compare(a_points.span(), b_points.span());
}

// ---- Tailorings.

ErrorOr<void> Collator::apply_rules(StringView rules)
{
    auto const code_points = TRY(Unicode::code_points_of(rules));
    auto const text = code_points.span();
    size_t at = 0;

    auto const skip_space = [&] {
        while (at < text.size() && Unicode::is_white_space(text[at]))
            ++at;
    };
    auto const is_syntax = [](CodePoint c) { return c == '&' || c == '<' || c == '=' || c == '/' || c == '|' || c == '[' || c == ']' || c == '\'' || c == '\\'; };

    // A run of what isn't the rules' own: quoted, or escaped, or as it is.
    auto const read_text = [&]() -> ErrorOr<Vector<CodePoint>> {
        Vector<CodePoint> out;
        skip_space();
        while (at < text.size()) {
            CodePoint const c = text[at];
            if (c == '\\' && at + 1 < text.size()) {
                TRY(out.try_append(text[at + 1]));
                at += 2;
                continue;
            }
            if (c == '\'') {
                ++at;
                while (at < text.size() && text[at] != '\'')
                    TRY(out.try_append(text[at++]));
                ++at;
                continue;
            }
            if (is_syntax(c) || Unicode::is_white_space(c))
                break;
            TRY(out.try_append(c));
            ++at;
        }
        return out;
    };

    Element current {};
    bool have_reset = false;
    // What comes between one thing and the next at the top level, so that each is a little further on than the last.
    u32 step = 0;

    while (at < text.size()) {
        skip_space();
        if (at >= text.size())
            break;
        if (text[at] == '[') {
            // An option. [reorder X], or something that doesn't change what order things are in.
            size_t const start = ++at;
            while (at < text.size() && text[at] != ']')
                ++at;
            auto const option = TRY(Unicode::utf8_of(text.slice(start, at - start)));
            ++at;
            if (option.view().starts_with("reorder "_sv)) {
                // Only for one script, moved to before Latin: which is all that the languages here want. Its range is from its letters.
                auto const names = option.view().substring_view(8);
                auto const name = names.substring_view(0, names.find(' ').value_or(names.length()));
                struct Sample {
                    StringView name;
                    CodePoint first;
                    CodePoint last;
                };
                Sample const samples[] = {
                    { "Cyrl"_sv, 0x430, 0x44f },
                    { "Grek"_sv, 0x3b1, 0x3c9 },
                    { "Arab"_sv, 0x621, 0x64a },
                    { "Hebr"_sv, 0x5d0, 0x5ea },
                    { "Deva"_sv, 0x904, 0x939 },
                };
                for (auto const& sample : samples) {
                    if (sample.name != name)
                        continue;
                    u32 low = 0xffffffff;
                    u32 high = 0;
                    for (CodePoint code = sample.first; code <= sample.last; ++code) {
                        Vector<Element> elements;
                        CodePoint const single[] = { code };
                        TRY(elements_for_rule_text({ single, 1 }, elements));
                        if (elements.is_empty() || elements[0].primary == 0)
                            continue;
                        low = min(low, elements[0].primary);
                        high = max(high, elements[0].primary);
                    }
                    Vector<Element> latin;
                    CodePoint const a[] = { 'a' };
                    TRY(elements_for_rule_text({ a, 1 }, latin));
                    m_moved_first = low;
                    m_moved_last = high + 0xff;
                    m_moved_to = latin[0].primary;
                }
            }
            continue;
        }
        if (text[at] == '&') {
            ++at;
            skip_space();
            bool before = false;
            if (at < text.size() && text[at] == '[') {
                size_t const start = ++at;
                while (at < text.size() && text[at] != ']')
                    ++at;
                auto const option = TRY(Unicode::utf8_of(text.slice(start, at - start)));
                ++at;
                before = option.view().starts_with("before"_sv);
            }
            auto const reset = TRY(read_text());
            Vector<Element> elements;
            TRY(elements_for_rule_text(reset.span(), elements));
            if (elements.is_empty())
                return Error::from_string_literal("a tailoring's reset has nothing in it");
            current = elements.last();
            have_reset = true;
            step = 0;
            if (before) {
                // Just before it: what follows goes in the gap below.
                current.primary = current.primary - 0x80;
                current.secondary = 0x20;
                current.tertiary = 0x02;
            }
            continue;
        }

        // A relation: <, <<, <<<, or =.
        int level;
        if (text[at] == '=') {
            level = 4;
            ++at;
        } else if (text[at] == '<') {
            level = 0;
            while (at < text.size() && text[at] == '<' && level < 3) {
                ++level;
                ++at;
            }
        } else {
            return Error::from_string_literal("a tailoring has something in it that isn't a rule");
        }
        if (!have_reset)
            return Error::from_string_literal("a tailoring starts with a relation");
        auto const related = TRY(read_text());
        // Anything to go on the end, like þ/h.
        Vector<CodePoint> extension;
        skip_space();
        if (at < text.size() && text[at] == '/') {
            ++at;
            extension = TRY(read_text());
        }

        switch (level) {
        case 1:
            current.primary += ++step;
            current.secondary = 0x20;
            current.tertiary = 0x02;
            break;
        case 2:
            current.secondary = static_cast<u16>(current.secondary + 1);
            current.tertiary = 0x02;
            break;
        case 3:
            current.tertiary = static_cast<u8>(current.tertiary + 1);
            break;
        default:
            break;
        }

        Tailored tailored;
        // By how it would be normalized, since it's what it's looked for in.
        tailored.key = TRY(Unicode::normalize(related.span(), Unicode::NormalizationForm::NFD));
        TRY(tailored.elements.try_append(current));
        if (!extension.is_empty())
            TRY(elements_for_rule_text(extension.span(), tailored.elements));
        TRY(m_tailored.try_append(move(tailored)));
    }
    return {};
}

ErrorOr<Collator> Collator::create(StringView language, Strength strength, Variable variable)
{
    Collator collator;
    collator.m_strength = strength;
    collator.m_variable = variable;
    if (!language.is_empty()) {
        auto const code = language.substring_view(0, language.find('_').value_or(language.find('-').value_or(language.length())));
        for (size_t i = 0; i < tailoring_rules_count; ++i) {
            if (StringView(tailoring_rules[i].language) == code) {
                TRY(collator.apply_rules(StringView(tailoring_rules[i].rules)));
                break;
            }
        }
    }
    return collator;
}

}
