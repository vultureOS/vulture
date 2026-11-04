/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/SpellChecker.h>

namespace UI {

namespace {

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

String lower_case(StringView word)
{
    StringBuilder out;
    for (size_t i = 0; i < word.length(); ++i)
        out.append(lower(word[i]));
    return MUST(out.to_string());
}

template<typename Each>
void for_each_line(StringView text, Each each)
{
    size_t start = 0;
    while (start < text.length()) {
        size_t end = start;
        while (end < text.length() && text[end] != '\n')
            ++end;
        size_t trimmed = end;
        while (trimmed > start && (text[trimmed - 1] == '\r' || text[trimmed - 1] == ' '))
            --trimmed;
        each(text.substring_view(start, trimmed - start));
        start = end + 1;
    }
}

Vector<StringView> fields_of(StringView line)
{
    Vector<StringView> fields;
    size_t at = 0;
    while (at < line.length()) {
        while (at < line.length() && (line[at] == ' ' || line[at] == '\t'))
            ++at;
        size_t const start = at;
        while (at < line.length() && line[at] != ' ' && line[at] != '\t')
            ++at;
        if (at > start)
            fields.append(line.substring_view(start, at - start));
    }
    return fields;
}

}

void SpellChecker::load(StringView dictionary, StringView affixes)
{
    // Kept, as the rules point into it.
    MUST(m_storage.try_resize(affixes.length()));
    if (affixes.length() > 0)
        __builtin_memcpy(m_storage.data(), affixes.characters_without_null_termination(), affixes.length());
    StringView const kept_affixes { m_storage.data(), m_storage.size() };

    for_each_line(kept_affixes, [&](StringView line) {
        auto const fields = fields_of(line);
        if (fields.size() < 4 || (fields[0] != "SFX"_sv && fields[0] != "PFX"_sv) || fields[1].length() != 1)
            return;
        // SFX A Y 3 is the heading of a set: SFX A 0 s . is one of it.
        if (fields.size() == 4)
            return;
        Affix affix;
        affix.is_prefix = fields[0] == "PFX"_sv;
        affix.flag = fields[1][0];
        affix.cross_product = true;
        affix.strip = fields[2] == "0"_sv ? StringView {} : fields[2];
        auto add = fields[3];
        // (Flags after a slash are for chains of rules, which this doesn't follow.)
        if (auto const slash = add.find('/'); slash.has_value())
            add = add.substring_view(0, slash.value());
        affix.add = add == "0"_sv ? StringView {} : add;
        affix.condition = fields.size() > 4 ? fields[4] : "."_sv;
        m_affixes.append(affix);
    });

    bool first = true;
    for_each_line(dictionary, [&](StringView line) {
        // The first line says how many there are.
        if (first) {
            first = false;
            return;
        }
        if (line.is_empty())
            return;
        auto const slash = line.find('/');
        auto const word = slash.has_value() ? line.substring_view(0, slash.value()) : line;
        auto flags = slash.has_value() ? line.substring_view(slash.value() + 1) : StringView {};
        // Anything after a tab isn't part of it.
        if (auto const tab = flags.find('\t'); tab.has_value())
            flags = flags.substring_view(0, tab.value());
        (void)m_words.try_set(lower_case(word), MUST(String::from_view(flags)));
    });
}

bool SpellChecker::condition_matches(StringView condition, StringView text, bool at_end)
{
    if (condition == "."_sv)
        return true;
    // Each position is a letter, a dot, or a bracket: read into how many positions it has first.
    Vector<StringView> positions;
    for (size_t at = 0; at < condition.length();) {
        if (condition[at] == '[') {
            size_t end = at;
            while (end < condition.length() && condition[end] != ']')
                ++end;
            positions.append(condition.substring_view(at, min(end + 1, condition.length()) - at));
            at = end + 1;
        } else {
            positions.append(condition.substring_view(at, 1));
            ++at;
        }
    }
    if (positions.size() > text.length())
        return false;
    size_t const start = at_end ? text.length() - positions.size() : 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        auto const position = positions[i];
        char const c = text[start + i];
        if (position == "."_sv)
            continue;
        if (position[0] == '[') {
            bool const negated = position.length() > 1 && position[1] == '^';
            bool found = false;
            for (size_t k = negated ? 2 : 1; k + 1 < position.length(); ++k)
                found = found || position[k] == c;
            if (found == negated)
                return false;
        } else if (position[0] != c) {
            return false;
        }
    }
    return true;
}

bool SpellChecker::is_known_form(StringView word) const
{
    auto const flags_of = [&](StringView stem) -> Optional<StringView> {
        auto const* found = m_words.get(MUST(String::from_view(stem)));
        if (!found)
            return {};
        return found->view();
    };
    if (flags_of(word).has_value())
        return true;
    for (auto const& affix : m_affixes) {
        if (affix.add.length() >= word.length() && !(affix.add.is_empty()))
            continue;
        if (affix.is_prefix) {
            if (!word.starts_with(affix.add))
                continue;
            StringBuilder stem;
            stem.append(affix.strip);
            stem.append(word.substring_view(affix.add.length()));
            auto const stem_text = MUST(stem.to_string());
            if (!condition_matches(affix.condition, stem_text.view(), false))
                continue;
            auto const flags = flags_of(stem_text.view());
            if (flags.has_value() && flags->find(affix.flag).has_value())
                return true;
        } else {
            if (!word.ends_with(affix.add))
                continue;
            StringBuilder stem;
            stem.append(word.substring_view(0, word.length() - affix.add.length()));
            stem.append(affix.strip);
            auto const stem_text = MUST(stem.to_string());
            if (!condition_matches(affix.condition, stem_text.view(), true))
                continue;
            auto const flags = flags_of(stem_text.view());
            if (flags.has_value() && flags->find(affix.flag).has_value())
                return true;
        }
    }
    return false;
}

bool SpellChecker::is_correct(StringView word) const
{
    if (m_words.is_empty() || word.is_empty())
        return true;
    // Only for what's made of ASCII letters and apostrophes: this is a dictionary for English, so far.
    bool has_letter = false;
    for (size_t i = 0; i < word.length(); ++i) {
        char const c = word[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            has_letter = true;
        else if (c != '\'')
            return true;
    }
    if (!has_letter)
        return true;
    auto const lowered = lower_case(word);
    if (is_known_form(lowered.view()))
        return true;
    // A possessive's the word.
    if (lowered.view().ends_with("'s"_sv) && is_known_form(lowered.view().substring_view(0, lowered.view().length() - 2)))
        return true;
    return false;
}

Vector<String> SpellChecker::suggestions(StringView word, size_t most) const
{
    Vector<String> found;
    if (m_words.is_empty() || word.length() > 40)
        return found;
    auto const lowered = lower_case(word);
    auto const text = lowered.view();
    auto const consider = [&](StringBuilder& candidate) {
        auto value = MUST(candidate.to_string());
        if (found.size() >= most)
            return;
        if (value.view() == text || !is_known_form(value.view()))
            return;
        for (auto const& existing : found) {
            if (existing.view() == value.view())
                return;
        }
        found.append(move(value));
    };
    // Two letters the wrong way round is the commonest of all.
    for (size_t i = 0; i + 1 < text.length(); ++i) {
        StringBuilder candidate;
        candidate.append(text.substring_view(0, i));
        candidate.append(text[i + 1]);
        candidate.append(text[i]);
        candidate.append(text.substring_view(i + 2));
        consider(candidate);
    }
    // Then one too many.
    for (size_t i = 0; i < text.length(); ++i) {
        StringBuilder candidate;
        candidate.append(text.substring_view(0, i));
        candidate.append(text.substring_view(i + 1));
        consider(candidate);
    }
    // One wrong.
    for (size_t i = 0; i < text.length(); ++i) {
        for (char c = 'a'; c <= 'z'; ++c) {
            if (c == text[i])
                continue;
            StringBuilder candidate;
            candidate.append(text.substring_view(0, i));
            candidate.append(c);
            candidate.append(text.substring_view(i + 1));
            consider(candidate);
        }
    }
    // One missing.
    for (size_t i = 0; i <= text.length(); ++i) {
        for (char c = 'a'; c <= 'z'; ++c) {
            StringBuilder candidate;
            candidate.append(text.substring_view(0, i));
            candidate.append(c);
            candidate.append(text.substring_view(i));
            consider(candidate);
        }
    }
    return found;
}

}
