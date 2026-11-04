/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/HashMap.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// Whether a word's spelled right, from a dictionary and its rules for how words change (the usual .dic and .aff files), and what it might have been instead.

namespace UI {

class SpellChecker {
public:
    // What the two files say. Nothing's kept of them but what's been made of them.
    void load(StringView dictionary, StringView affixes);
    [[nodiscard]] bool is_loaded() const { return !m_words.is_empty(); }

    // Anything that's not a word (a number, a name in an unknown script) is right: there's nothing to say about it.
    [[nodiscard]] bool is_correct(StringView word) const;
    // In order of how likely: what's one mistake away first.
    [[nodiscard]] Vector<String> suggestions(StringView word, size_t most = 5) const;

private:
    struct Affix {
        char flag { 0 };
        bool is_prefix { false };
        bool cross_product { false };
        StringView strip;
        StringView add;
        // Reads as a simple pattern: letters, [abc] and [^abc].
        StringView condition;
    };

    [[nodiscard]] bool is_known_form(StringView lower_case_word) const;
    [[nodiscard]] static bool condition_matches(StringView condition, StringView text, bool at_end);

    Vector<char> m_storage;
    HashMap<String, String> m_words;
    Vector<Affix> m_affixes;
};

}
