/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/InputMethod.h>
#include <fcntl.h>
#include <unistd.h>

// Pinyin, Kana and Hangul. Each takes Latin letters off the keyboard (as a Mac's do), and makes characters of them.

namespace UI {

namespace {

constexpr u16 key_backspace = 14;
constexpr u16 key_enter = 28;
constexpr u16 key_escape = 1;
constexpr u16 key_space = 57;
constexpr u16 key_up = 103;
constexpr u16 key_down = 108;

Vector<char> read_table(char const* name)
{
    StringBuilder path;
    path.append("/System/Library/Input Methods/"_sv);
    path.append(StringView(name));
    auto const full_path = MUST(path.to_string());
    Vector<char> bytes;
    int const fd = open(full_path.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return bytes;
    char chunk[16384];
    for (;;) {
        ssize_t const got = read(fd, chunk, sizeof(chunk));
        if (got <= 0)
            break;
        size_t const at = bytes.size();
        if (bytes.try_resize(at + static_cast<size_t>(got)).is_error())
            break;
        __builtin_memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return bytes;
}

// A table with a line for each reading, sorted by it: "ni 你泥...". Binary search over lines isn't worth it: there are a few thousand and they're only looked at when a key's pressed,
// so an index of where each line is will do.
class ReadingTable {
public:
    explicit ReadingTable(char const* name)
        : m_bytes(read_table(name))
    {
        size_t start = 0;
        StringView const all { m_bytes.data(), m_bytes.size() };
        while (start < all.length()) {
            size_t end = start;
            while (end < all.length() && all[end] != '\n')
                ++end;
            size_t space = start;
            while (space < end && all[space] != ' ')
                ++space;
            if (space < end)
                m_lines.append({ all.substring_view(start, space - start), all.substring_view(space + 1, end - space - 1) });
            start = end + 1;
        }
    }

    [[nodiscard]] StringView lookup(StringView reading) const
    {
        for (auto const& line : m_lines) {
            if (line.reading == reading)
                return line.characters;
        }
        return {};
    }
    [[nodiscard]] bool has_prefix(StringView prefix) const
    {
        for (auto const& line : m_lines) {
            if (line.reading.starts_with(prefix))
                return true;
        }
        return false;
    }

private:
    struct Line {
        StringView reading;
        StringView characters;
    };
    Vector<char> m_bytes;
    Vector<Line> m_lines;
};

// One whole character's bytes, from a run of them.
size_t utf8_length_at(StringView text, size_t at)
{
    auto const lead = static_cast<u8>(text[at]);
    return lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
}

void choose_candidates_from(StringView characters, Vector<String>& out, size_t most)
{
    for (size_t at = 0; at < characters.length() && out.size() < most;) {
        size_t const length = utf8_length_at(characters, at);
        out.append(MUST(String::from_view(characters.substring_view(at, length))));
        at += length;
    }
}

constexpr size_t most_candidates = 9;

// What's common to anything with a list to choose from.
class ChoosingInputMethod : public InputMethod {
public:
    [[nodiscard]] Span<String const> candidates() const override { return m_candidates.span(); }
    [[nodiscard]] size_t selected_candidate() const override { return m_selected; }

protected:
    // A number, an arrow or space: which of the candidates.
    [[nodiscard]] Optional<Result> handle_choosing(KeyEvent const& event)
    {
        if (m_candidates.is_empty())
            return {};
        if (event.code == key_down) {
            m_selected = (m_selected + 1) % m_candidates.size();
            return Result { true, {} };
        }
        if (event.code == key_up) {
            m_selected = (m_selected + m_candidates.size() - 1) % m_candidates.size();
            return Result { true, {} };
        }
        if (event.text.length() == 1 && event.text[0] >= '1' && event.text[0] <= '9') {
            size_t const index = static_cast<size_t>(event.text[0] - '1');
            if (index < m_candidates.size()) {
                auto const chosen = m_candidates[index];
                return chose(chosen);
            }
        }
        if (event.code == key_space) {
            auto const chosen = m_candidates[m_selected];
            return chose(chosen);
        }
        return {};
    }

    virtual Result chose(String const&) = 0;

    Vector<String> m_candidates;
    size_t m_selected { 0 };
};

class PinyinInputMethod final : public ChoosingInputMethod {
public:
    PinyinInputMethod()
        : m_table("Pinyin.table")
    {
    }

    Result handle_key(KeyEvent const& event) override
    {
        if (event.has_command() || event.has_control() || event.has_option())
            return {};
        if (!m_typed.is_empty()) {
            if (auto choosing = handle_choosing(event); choosing.has_value())
                return choosing.release_value();
            if (event.code == key_backspace) {
                m_typed.take_last();
                update();
                return { true, {} };
            }
            if (event.code == key_escape) {
                reset();
                return { true, {} };
            }
            if (event.code == key_enter) {
                // As the letters that they are.
                auto const letters = typed();
                reset();
                return { true, letters };
            }
        }
        if (event.text.length() == 1 && event.text[0] >= 'a' && event.text[0] <= 'z') {
            m_typed.append(event.text[0]);
            update();
            return { true, {} };
        }
        if (!m_typed.is_empty()) {
            // Anything else ends it, with the best guess, then whatever it was.
            if (!event.text.is_empty()) {
                StringBuilder out;
                if (!m_candidates.is_empty())
                    out.append(m_candidates[0].view());
                else
                    out.append(typed().view());
                out.append(event.text);
                reset();
                return { true, MUST(out.to_string()) };
            }
            return { true, {} };
        }
        return {};
    }

    [[nodiscard]] StringView marked_text() const override { return m_marked.view(); }

    void reset() override
    {
        m_typed.clear();
        m_candidates.clear();
        m_marked = MUST(String::from_view(""_sv));
        m_syllables_chosen = 0;
        m_selected = 0;
    }

private:
    [[nodiscard]] String typed() const { return MUST(String::from_view(StringView(m_typed.data(), m_typed.size()))); }

    Result chose(String const& chosen) override
    {
        // If that was all of it, that's it. If it was only the first syllable, the rest are still to be decided.
        if (chosen.view() == m_whole_guess.view() || m_syllables.size() <= 1) {
            auto const text = chosen;
            reset();
            return { true, text };
        }
        auto const rest_start = m_syllables[0].length();
        Vector<char> rest;
        for (size_t i = rest_start; i < m_typed.size(); ++i)
            rest.append(m_typed[i]);
        m_typed = move(rest);
        update();
        return { true, chosen };
    }

    // Letters into syllables, each as long as it can be.
    void update()
    {
        m_syllables.clear();
        m_candidates.clear();
        m_selected = 0;
        StringView const all { m_typed.data(), m_typed.size() };
        for (size_t at = 0; at < all.length();) {
            size_t best = 0;
            for (size_t length = min<size_t>(6, all.length() - at); length > 0; --length) {
                if (!m_table.lookup(all.substring_view(at, length)).is_empty()) {
                    best = length;
                    break;
                }
            }
            if (best == 0) {
                // Not a syllable yet, or ever: what's left is shown as it is.
                m_syllables.append(all.substring_view(at));
                break;
            }
            m_syllables.append(all.substring_view(at, best));
            at += best;
        }
        // What's shown in the text is the letters, with a space between syllables, as on a Mac.
        StringBuilder marked;
        for (size_t i = 0; i < m_syllables.size(); ++i) {
            if (i)
                marked.append(' ');
            marked.append(m_syllables[i]);
        }
        m_marked = MUST(marked.to_string());

        // The first guess is the most used character for each, for all of it; then the choices for the first.
        StringBuilder whole;
        bool all_known = true;
        for (auto const& syllable : m_syllables) {
            auto const characters = m_table.lookup(syllable);
            if (characters.is_empty()) {
                all_known = false;
                break;
            }
            whole.append(characters.substring_view(0, utf8_length_at(characters, 0)));
        }
        if (all_known && m_syllables.size() > 1) {
            m_whole_guess = MUST(whole.to_string());
            m_candidates.append(m_whole_guess);
        } else {
            m_whole_guess = MUST(String::from_view(""_sv));
        }
        if (!m_syllables.is_empty())
            choose_candidates_from(m_table.lookup(m_syllables[0]), m_candidates, most_candidates);
    }

    ReadingTable m_table;
    Vector<char> m_typed;
    Vector<StringView> m_syllables;
    String m_marked;
    String m_whole_guess;
    size_t m_syllables_chosen { 0 };
};

// Latin letters to kana, as they're written by a Mac's Romaji: longest match, and a doubled consonant is the small tsu.
struct RomajiEntry {
    StringView romaji;
    StringView kana;
};

constexpr RomajiEntry s_romaji[] = {
    { "kya"_sv, "\xe3\x81\x8d\xe3\x82\x83"_sv }, { "kyu"_sv, "\xe3\x81\x8d\xe3\x82\x85"_sv }, { "kyo"_sv, "\xe3\x81\x8d\xe3\x82\x87"_sv },
    { "sha"_sv, "\xe3\x81\x97\xe3\x82\x83"_sv }, { "shu"_sv, "\xe3\x81\x97\xe3\x82\x85"_sv }, { "sho"_sv, "\xe3\x81\x97\xe3\x82\x87"_sv }, { "shi"_sv, "\xe3\x81\x97"_sv },
    { "sya"_sv, "\xe3\x81\x97\xe3\x82\x83"_sv }, { "syu"_sv, "\xe3\x81\x97\xe3\x82\x85"_sv }, { "syo"_sv, "\xe3\x81\x97\xe3\x82\x87"_sv },
    { "cha"_sv, "\xe3\x81\xa1\xe3\x82\x83"_sv }, { "chu"_sv, "\xe3\x81\xa1\xe3\x82\x85"_sv }, { "cho"_sv, "\xe3\x81\xa1\xe3\x82\x87"_sv }, { "chi"_sv, "\xe3\x81\xa1"_sv },
    { "tya"_sv, "\xe3\x81\xa1\xe3\x82\x83"_sv }, { "tyu"_sv, "\xe3\x81\xa1\xe3\x82\x85"_sv }, { "tyo"_sv, "\xe3\x81\xa1\xe3\x82\x87"_sv }, { "tsu"_sv, "\xe3\x81\xa4"_sv },
    { "nya"_sv, "\xe3\x81\xab\xe3\x82\x83"_sv }, { "nyu"_sv, "\xe3\x81\xab\xe3\x82\x85"_sv }, { "nyo"_sv, "\xe3\x81\xab\xe3\x82\x87"_sv },
    { "hya"_sv, "\xe3\x81\xb2\xe3\x82\x83"_sv }, { "hyu"_sv, "\xe3\x81\xb2\xe3\x82\x85"_sv }, { "hyo"_sv, "\xe3\x81\xb2\xe3\x82\x87"_sv },
    { "mya"_sv, "\xe3\x81\xbf\xe3\x82\x83"_sv }, { "myu"_sv, "\xe3\x81\xbf\xe3\x82\x85"_sv }, { "myo"_sv, "\xe3\x81\xbf\xe3\x82\x87"_sv },
    { "rya"_sv, "\xe3\x82\x8a\xe3\x82\x83"_sv }, { "ryu"_sv, "\xe3\x82\x8a\xe3\x82\x85"_sv }, { "ryo"_sv, "\xe3\x82\x8a\xe3\x82\x87"_sv },
    { "gya"_sv, "\xe3\x81\x8e\xe3\x82\x83"_sv }, { "gyu"_sv, "\xe3\x81\x8e\xe3\x82\x85"_sv }, { "gyo"_sv, "\xe3\x81\x8e\xe3\x82\x87"_sv },
    { "bya"_sv, "\xe3\x81\xb3\xe3\x82\x83"_sv }, { "byu"_sv, "\xe3\x81\xb3\xe3\x82\x85"_sv }, { "byo"_sv, "\xe3\x81\xb3\xe3\x82\x87"_sv },
    { "pya"_sv, "\xe3\x81\xb4\xe3\x82\x83"_sv }, { "pyu"_sv, "\xe3\x81\xb4\xe3\x82\x85"_sv }, { "pyo"_sv, "\xe3\x81\xb4\xe3\x82\x87"_sv },
    { "ja"_sv, "\xe3\x81\x98\xe3\x82\x83"_sv }, { "ju"_sv, "\xe3\x81\x98\xe3\x82\x85"_sv }, { "jo"_sv, "\xe3\x81\x98\xe3\x82\x87"_sv }, { "ji"_sv, "\xe3\x81\x98"_sv },
    { "ka"_sv, "\xe3\x81\x8b"_sv }, { "ki"_sv, "\xe3\x81\x8d"_sv }, { "ku"_sv, "\xe3\x81\x8f"_sv }, { "ke"_sv, "\xe3\x81\x91"_sv }, { "ko"_sv, "\xe3\x81\x93"_sv },
    { "ga"_sv, "\xe3\x81\x8c"_sv }, { "gi"_sv, "\xe3\x81\x8e"_sv }, { "gu"_sv, "\xe3\x81\x90"_sv }, { "ge"_sv, "\xe3\x81\x92"_sv }, { "go"_sv, "\xe3\x81\x94"_sv },
    { "sa"_sv, "\xe3\x81\x95"_sv }, { "si"_sv, "\xe3\x81\x97"_sv }, { "su"_sv, "\xe3\x81\x99"_sv }, { "se"_sv, "\xe3\x81\x9b"_sv }, { "so"_sv, "\xe3\x81\x9d"_sv },
    { "za"_sv, "\xe3\x81\x96"_sv }, { "zi"_sv, "\xe3\x81\x98"_sv }, { "zu"_sv, "\xe3\x81\x9a"_sv }, { "ze"_sv, "\xe3\x81\x9c"_sv }, { "zo"_sv, "\xe3\x81\x9e"_sv },
    { "ta"_sv, "\xe3\x81\x9f"_sv }, { "ti"_sv, "\xe3\x81\xa1"_sv }, { "tu"_sv, "\xe3\x81\xa4"_sv }, { "te"_sv, "\xe3\x81\xa6"_sv }, { "to"_sv, "\xe3\x81\xa8"_sv },
    { "da"_sv, "\xe3\x81\xa0"_sv }, { "di"_sv, "\xe3\x81\xa2"_sv }, { "du"_sv, "\xe3\x81\xa5"_sv }, { "de"_sv, "\xe3\x81\xa7"_sv }, { "do"_sv, "\xe3\x81\xa9"_sv },
    { "na"_sv, "\xe3\x81\xaa"_sv }, { "ni"_sv, "\xe3\x81\xab"_sv }, { "nu"_sv, "\xe3\x81\xac"_sv }, { "ne"_sv, "\xe3\x81\xad"_sv }, { "no"_sv, "\xe3\x81\xae"_sv },
    { "ha"_sv, "\xe3\x81\xaf"_sv }, { "hi"_sv, "\xe3\x81\xb2"_sv }, { "fu"_sv, "\xe3\x81\xb5"_sv }, { "hu"_sv, "\xe3\x81\xb5"_sv }, { "he"_sv, "\xe3\x81\xb8"_sv }, { "ho"_sv, "\xe3\x81\xbb"_sv },
    { "ba"_sv, "\xe3\x81\xb0"_sv }, { "bi"_sv, "\xe3\x81\xb3"_sv }, { "bu"_sv, "\xe3\x81\xb6"_sv }, { "be"_sv, "\xe3\x81\xb9"_sv }, { "bo"_sv, "\xe3\x81\xbc"_sv },
    { "pa"_sv, "\xe3\x81\xb1"_sv }, { "pi"_sv, "\xe3\x81\xb4"_sv }, { "pu"_sv, "\xe3\x81\xb7"_sv }, { "pe"_sv, "\xe3\x81\xba"_sv }, { "po"_sv, "\xe3\x81\xbd"_sv },
    { "ma"_sv, "\xe3\x81\xbe"_sv }, { "mi"_sv, "\xe3\x81\xbf"_sv }, { "mu"_sv, "\xe3\x82\x80"_sv }, { "me"_sv, "\xe3\x82\x81"_sv }, { "mo"_sv, "\xe3\x82\x82"_sv },
    { "ya"_sv, "\xe3\x82\x84"_sv }, { "yu"_sv, "\xe3\x82\x86"_sv }, { "yo"_sv, "\xe3\x82\x88"_sv },
    { "ra"_sv, "\xe3\x82\x89"_sv }, { "ri"_sv, "\xe3\x82\x8a"_sv }, { "ru"_sv, "\xe3\x82\x8b"_sv }, { "re"_sv, "\xe3\x82\x8c"_sv }, { "ro"_sv, "\xe3\x82\x8d"_sv },
    { "wa"_sv, "\xe3\x82\x8f"_sv }, { "wo"_sv, "\xe3\x82\x92"_sv }, { "nn"_sv, "\xe3\x82\x93"_sv },
    { "a"_sv, "\xe3\x81\x82"_sv }, { "i"_sv, "\xe3\x81\x84"_sv }, { "u"_sv, "\xe3\x81\x86"_sv }, { "e"_sv, "\xe3\x81\x88"_sv }, { "o"_sv, "\xe3\x81\x8a"_sv },
};

class KanaInputMethod final : public ChoosingInputMethod {
public:
    KanaInputMethod()
        : m_table("Kana.table")
    {
    }

    Result handle_key(KeyEvent const& event) override
    {
        if (event.has_command() || event.has_control() || event.has_option())
            return {};
        bool const composing = !m_kana.is_empty() || !m_pending.is_empty();
        if (composing) {
            if (m_converting) {
                if (auto choosing = handle_choosing(event); choosing.has_value())
                    return choosing.release_value();
            } else if (event.code == key_space) {
                start_converting();
                return { true, {} };
            }
            if (event.code == key_enter) {
                auto text = current_text();
                reset();
                return { true, text };
            }
            if (event.code == key_escape) {
                reset();
                return { true, {} };
            }
            if (event.code == key_backspace) {
                m_converting = false;
                m_candidates.clear();
                if (!m_pending.is_empty()) {
                    m_pending.take_last();
                } else {
                    // A whole character: they're all three bytes here.
                    for (int i = 0; i < 3 && !m_kana.is_empty(); ++i)
                        m_kana.take_last();
                }
                update_marked();
                return { true, {} };
            }
        }
        if (event.text.length() == 1 && event.text[0] >= 'a' && event.text[0] <= 'z') {
            if (m_converting) {
                auto text = current_text();
                reset();
                m_pending.append(event.text[0]);
                convert_pending();
                return { true, text };
            }
            m_pending.append(event.text[0]);
            convert_pending();
            return { true, {} };
        }
        if (composing && !event.text.is_empty()) {
            StringBuilder out;
            out.append(current_text().view());
            out.append(event.text);
            reset();
            return { true, MUST(out.to_string()) };
        }
        return composing ? Result { true, {} } : Result {};
    }

    [[nodiscard]] StringView marked_text() const override { return m_marked.view(); }

    void reset() override
    {
        m_kana.clear();
        m_pending.clear();
        m_candidates.clear();
        m_converting = false;
        m_selected = 0;
        m_marked = MUST(String::from_view(""_sv));
    }

private:
    [[nodiscard]] String current_text() const
    {
        if (m_converting && !m_candidates.is_empty())
            return m_candidates[m_selected];
        StringBuilder out;
        out.append(StringView(m_kana.data(), m_kana.size()));
        // What's been typed that didn't come to anything: a lone n is the one that's obvious.
        StringView const pending { m_pending.data(), m_pending.size() };
        if (pending == "n"_sv)
            out.append("\xe3\x82\x93"_sv);
        else
            out.append(pending);
        return MUST(out.to_string());
    }

    void append_kana(StringView kana)
    {
        for (size_t i = 0; i < kana.length(); ++i)
            m_kana.append(kana[i]);
    }

    void convert_pending()
    {
        for (;;) {
            StringView const pending { m_pending.data(), m_pending.size() };
            if (pending.is_empty())
                break;
            bool found = false;
            for (auto const& entry : s_romaji) {
                if (pending == entry.romaji) {
                    append_kana(entry.kana);
                    m_pending.clear();
                    found = true;
                    break;
                }
            }
            if (found)
                break;
            // A doubled consonant: the small tsu, and the second's still to come.
            if (pending.length() >= 2 && pending[0] == pending[1] && pending[0] != 'n' && pending[0] != 'a' && pending[0] != 'i' && pending[0] != 'u' && pending[0] != 'e' && pending[0] != 'o') {
                append_kana("\xe3\x81\xa3"_sv);
                m_pending.remove(0);
                continue;
            }
            // n before a consonant that can't follow it in a syllable is its own.
            if (pending.length() >= 2 && pending[0] == 'n' && pending[1] != 'y' && pending[1] != 'a' && pending[1] != 'i' && pending[1] != 'u' && pending[1] != 'e' && pending[1] != 'o') {
                append_kana("\xe3\x82\x93"_sv);
                m_pending.remove(0);
                continue;
            }
            // Could more make it something? If not, it's what's first that isn't anything.
            bool could_be = false;
            for (auto const& entry : s_romaji) {
                if (entry.romaji.starts_with(pending))
                    could_be = true;
            }
            if (could_be || pending.length() == 1)
                break;
            m_pending.remove(0);
        }
        update_marked();
    }

    void update_marked()
    {
        m_marked = current_text();
    }

    void start_converting()
    {
        auto const whole = current_text();
        m_kana.clear();
        for (size_t i = 0; i < whole.view().length(); ++i)
            m_kana.append(whole.view()[i]);
        m_pending.clear();
        m_candidates.clear();
        m_selected = 0;
        // Its characters, then itself, then in katakana.
        m_candidates.append(whole);
        choose_candidates_from(m_table.lookup(whole.view()), m_candidates, most_candidates - 2);
        StringBuilder katakana;
        auto const text = whole.view();
        for (size_t at = 0; at + 2 < text.length() + 1 && at < text.length();) {
            size_t const length = utf8_length_at(text, at);
            if (length == 3 && static_cast<u8>(text[at]) == 0xe3) {
                // Each hiragana is sixty before its katakana.
                u32 code = ((static_cast<u32>(text[at]) & 0xf) << 12) | ((static_cast<u32>(text[at + 1]) & 0x3f) << 6) | (static_cast<u32>(text[at + 2]) & 0x3f);
                if (code >= 0x3041 && code <= 0x3096)
                    code += 0x60;
                katakana.append_code_point(code);
            } else {
                katakana.append(text.substring_view(at, length));
            }
            at += length;
        }
        m_candidates.append(MUST(katakana.to_string()));
        m_converting = true;
        if (m_candidates.size() > 2)
            m_selected = 1;
        m_marked = m_candidates[m_selected];
    }

    Result chose(String const& chosen) override
    {
        auto const text = chosen;
        reset();
        return { true, text };
    }

    ReadingTable m_table;
    Vector<char> m_kana;
    Vector<char> m_pending;
    String m_marked;
    bool m_converting { false };
};

// Two-set Hangul: the keys are the letters, and they're put together into syllables as they come.
class HangulInputMethod final : public InputMethod {
public:
    Result handle_key(KeyEvent const& event) override
    {
        if (event.has_command() || event.has_control() || event.has_option())
            return {};
        if (event.code == key_backspace && has_syllable()) {
            take_back();
            return { true, {} };
        }
        if (event.text.length() == 1) {
            char const c = event.text[0];
            int const jamo = jamo_for_key(c);
            if (jamo != 0) {
                String commit;
                add(jamo, commit);
                return { true, commit };
            }
        }
        if (has_syllable()) {
            // Anything else ends it, and goes on to do whatever it does.
            StringBuilder out;
            out.append(m_marked.view());
            if (!event.text.is_empty() && event.code != key_enter && event.code != key_backspace)
                out.append(event.text);
            bool const pass_on = event.code == key_enter || event.text.is_empty();
            reset();
            if (pass_on) {
                m_pending_commit = MUST(out.to_string());
                return { false, m_pending_commit };
            }
            return { true, MUST(out.to_string()) };
        }
        return {};
    }

    [[nodiscard]] StringView marked_text() const override { return m_marked.view(); }

    void reset() override
    {
        m_initial = m_medial = m_final = -1;
        m_marked = MUST(String::from_view(""_sv));
    }

private:
    [[nodiscard]] bool has_syllable() const { return m_initial >= 0 || m_medial >= 0; }

    static int jamo_for_key(char c)
    {
        // The standard two-set layout, by Latin letter.
        switch (c) {
        case 'q': return 0x3142;
        case 'Q': return 0x3143;
        case 'w': return 0x3148;
        case 'W': return 0x3149;
        case 'e': return 0x3137;
        case 'E': return 0x3138;
        case 'r': return 0x3131;
        case 'R': return 0x3132;
        case 't': return 0x3145;
        case 'T': return 0x3146;
        case 'y': return 0x315b;
        case 'u': return 0x3155;
        case 'i': return 0x3151;
        case 'o': return 0x3150;
        case 'O': return 0x3152;
        case 'p': return 0x3154;
        case 'P': return 0x3156;
        case 'a': return 0x3141;
        case 's': return 0x3134;
        case 'd': return 0x3147;
        case 'f': return 0x3139;
        case 'g': return 0x314e;
        case 'h': return 0x3157;
        case 'j': return 0x3153;
        case 'k': return 0x314f;
        case 'l': return 0x3163;
        case 'z': return 0x314b;
        case 'x': return 0x314c;
        case 'c': return 0x314a;
        case 'v': return 0x314d;
        case 'b': return 0x3160;
        case 'n': return 0x315c;
        case 'm': return 0x3161;
        default: return 0;
        }
    }

    static int initial_index(int jamo)
    {
        constexpr int initials[] = { 0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145, 0x3146, 0x3147, 0x3148, 0x3149, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e };
        for (int i = 0; i < 19; ++i) {
            if (initials[i] == jamo)
                return i;
        }
        return -1;
    }

    static int medial_index(int jamo)
    {
        return jamo >= 0x314f && jamo <= 0x3163 ? jamo - 0x314f : -1;
    }

    static int final_index(int jamo)
    {
        constexpr int finals[] = { 0, 0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313a, 0x313b, 0x313c, 0x313d, 0x313e, 0x313f, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145, 0x3146, 0x3147, 0x3148, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e };
        for (int i = 1; i < 28; ++i) {
            if (finals[i] == jamo)
                return i;
        }
        return -1;
    }

    static int final_to_jamo(int index)
    {
        constexpr int finals[] = { 0, 0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313a, 0x313b, 0x313c, 0x313d, 0x313e, 0x313f, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145, 0x3146, 0x3147, 0x3148, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e };
        return finals[index];
    }

    // Two vowels that go together into one.
    static int combined_medial(int first, int second)
    {
        struct Pair {
            int first, second, combined;
        };
        // In the list of medials: 8 is o, 13 is u, 18 is eu.
        constexpr Pair pairs[] = { { 8, 0, 9 }, { 8, 1, 10 }, { 8, 20, 11 }, { 13, 4, 14 }, { 13, 5, 15 }, { 13, 20, 16 }, { 18, 20, 19 } };
        for (auto const& pair : pairs) {
            if (pair.first == first && pair.second == second)
                return pair.combined;
        }
        return -1;
    }

    // Two consonants that end a syllable together. Finals are in order of `final_index`.
    static int combined_final(int first, int jamo)
    {
        struct Pair {
            int first, jamo, combined;
        };
        constexpr Pair pairs[] = {
            { 1, 0x3145, 3 }, { 4, 0x3148, 5 }, { 4, 0x314e, 6 }, { 8, 0x3131, 9 }, { 8, 0x3141, 10 }, { 8, 0x3142, 11 }, { 8, 0x3145, 12 }, { 8, 0x314c, 13 }, { 8, 0x314d, 14 }, { 8, 0x314e, 15 }, { 17, 0x3145, 18 },
        };
        for (auto const& pair : pairs) {
            if (pair.first == first && pair.jamo == jamo)
                return pair.combined;
        }
        return -1;
    }

    // And what a pair of finals comes apart into, when a vowel takes the second.
    static bool split_final(int combined, int& stays, int& moves)
    {
        struct Pair {
            int combined, stays, moves;
        };
        constexpr Pair pairs[] = {
            { 3, 1, 0x3145 }, { 5, 4, 0x3148 }, { 6, 4, 0x314e }, { 9, 8, 0x3131 }, { 10, 8, 0x3141 }, { 11, 8, 0x3142 }, { 12, 8, 0x3145 }, { 13, 8, 0x314c }, { 14, 8, 0x314d }, { 15, 8, 0x314e }, { 18, 17, 0x3145 },
        };
        for (auto const& pair : pairs) {
            if (pair.combined == combined) {
                stays = pair.stays;
                moves = pair.moves;
                return true;
            }
        }
        return false;
    }

    void add(int jamo, String& commit)
    {
        int const medial = medial_index(jamo);
        if (medial >= 0) {
            if (m_initial >= 0 && m_medial < 0) {
                m_medial = medial;
            } else if (m_medial >= 0 && m_final < 0) {
                int const combined = combined_medial(m_medial, medial);
                if (combined >= 0) {
                    m_medial = combined;
                } else {
                    commit = compose();
                    reset();
                    m_medial = medial;
                }
            } else if (m_final > 0) {
                // The last consonant wasn't the end of this syllable after all: it starts the next.
                int stays = 0;
                int moves = final_to_jamo(m_final);
                if (split_final(m_final, stays, moves)) {
                    m_final = stays;
                } else {
                    m_final = -1;
                }
                commit = compose();
                reset();
                m_initial = initial_index(moves);
                m_medial = medial;
            } else {
                // A vowel by itself.
                commit = compose();
                reset();
                m_medial = medial;
            }
        } else {
            if (m_initial >= 0 && m_medial >= 0 && m_final < 0) {
                int const as_final = final_index(jamo);
                if (as_final > 0) {
                    m_final = as_final;
                } else {
                    commit = compose();
                    reset();
                    m_initial = initial_index(jamo);
                }
            } else if (m_final > 0) {
                int const combined = combined_final(m_final, jamo);
                if (combined > 0) {
                    m_final = combined;
                } else {
                    commit = compose();
                    reset();
                    m_initial = initial_index(jamo);
                }
            } else if (m_initial < 0 && m_medial < 0) {
                m_initial = initial_index(jamo);
            } else {
                commit = compose();
                reset();
                m_initial = initial_index(jamo);
            }
        }
        m_marked = compose();
    }

    void take_back()
    {
        if (m_final > 0) {
            int stays = 0, moves = 0;
            m_final = split_final(m_final, stays, moves) ? stays : -1;
        } else if (m_medial >= 0) {
            m_medial = -1;
        } else {
            m_initial = -1;
        }
        m_marked = compose();
    }

    [[nodiscard]] String compose() const
    {
        StringBuilder out;
        if (m_initial >= 0 && m_medial >= 0) {
            u32 const syllable = 0xac00 + (static_cast<u32>(m_initial) * 21 + static_cast<u32>(m_medial)) * 28 + static_cast<u32>(m_final > 0 ? m_final : 0);
            out.append_code_point(syllable);
        } else if (m_initial >= 0) {
            constexpr int initials[] = { 0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145, 0x3146, 0x3147, 0x3148, 0x3149, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e };
            out.append_code_point(static_cast<u32>(initials[m_initial]));
        } else if (m_medial >= 0) {
            out.append_code_point(static_cast<u32>(0x314f + m_medial));
        }
        return MUST(out.to_string());
    }

    int m_initial { -1 };
    int m_medial { -1 };
    int m_final { -1 };
    String m_marked;
    String m_pending_commit;
};

}

OwnPtr<InputMethod> make_pinyin_input_method()
{
    return adopt_own(*new PinyinInputMethod);
}

OwnPtr<InputMethod> make_kana_input_method()
{
    return adopt_own(*new KanaInputMethod);
}

OwnPtr<InputMethod> make_hangul_input_method()
{
    return adopt_own(*new HangulInputMethod);
}

}
