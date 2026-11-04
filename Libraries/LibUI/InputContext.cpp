/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Appearance.h>
#include <LibUI/Controls.h>
#include <LibUI/Graphics.h>
#include <LibUI/InputMethod.h>
#include <LibUI/Layout.h>
#include <LibUI/ScrollView.h>
#include <LibUI/SpellChecker.h>
#include <LibUI/TextField.h>
#include <LibUI/TextStorage.h>
#include <LibUI/Window.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace UI {

namespace WS = ::Window;

using Gfx::FloatRect;

class CandidateList final : public View {
public:
    void set_candidates(Span<String const> candidates, size_t selected)
    {
        m_candidates.clear();
        for (auto const& candidate : candidates)
            m_candidates.append(candidate);
        m_selected = selected;
        set_needs_display();
    }

    static constexpr float row_height = 22;
    static constexpr float width_of_list = 180;

    void draw(Graphics& graphics) override
    {
        auto const& t = Appearance::the();
        FloatRect const all { 0, 0, width(), height() };
        graphics.fill(all, t.color("menu.tint"_sv));
        auto const font = t.named_font("menu"_sv);
        float const inset = t.metric("menu.selectionInset"_sv);
        for (size_t i = 0; i < m_candidates.size(); ++i) {
            float const y = static_cast<float>(i) * row_height;
            bool const selected = i == m_selected;
            if (selected)
                graphics.fill_rounded({ inset, y, width() - inset * 2, row_height }, t.metric("menu.selectionRadius"_sv), t.color("menu.selection"_sv));
            char number[4];
            snprintf(number, sizeof(number), "%zu", i + 1);
            graphics.draw_text_centered(StringView(number, strlen(number)), font, { 10, y, 14, row_height }, t.color(selected ? "menu.selectedText"_sv : "menu.shortcut"_sv), TextAlignment::Left);
            graphics.draw_text_centered(m_candidates[i].view(), font, { 28, y, width() - 32, row_height }, t.color(selected ? "menu.selectedText"_sv : "menu.text"_sv), TextAlignment::Left);
        }
        graphics.stroke_rounded(all, t.metric("menu.radius"_sv), graphics.pixel(), t.color("menu.border"_sv));
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Table; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Candidates"_sv)); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (size_t i = 0; i < m_candidates.size(); ++i) {
            FloatRect const rect { 0, static_cast<float>(i) * row_height, width(), row_height };
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Row, m_candidates[i].view(), rect, {}));
        }
    }

private:
    Vector<String> m_candidates;
    size_t m_selected { 0 };
};

class CandidateWindow {
public:
    CandidateWindow(Gfx::IntPoint at, size_t count)
        : m_window(adopt_own(*new Window("candidates"_sv, { at.x, at.y, static_cast<int>(CandidateList::width_of_list), static_cast<int>(CandidateList::row_height * static_cast<float>(count)) }, WS::WindowBare | WS::WindowPopup)))
    {
        m_window->set_transparent(true);
        m_window->backend().set_corner_radius(static_cast<int>(Appearance::the().metric("menu.radius"_sv)));
        m_list = &m_window->root_view().add<CandidateList>();
        m_list->set_frame({ 0, 0, CandidateList::width_of_list, CandidateList::row_height * static_cast<float>(count) });
        m_window->show();
    }

    [[nodiscard]] CandidateList& list() { return *m_list; }
    [[nodiscard]] size_t room() const { return static_cast<size_t>(m_list->height() / CandidateList::row_height); }

private:
    OwnPtr<Window> m_window;
    CandidateList* m_list { nullptr };
};

// ---- Press and hold: a letter's other forms, by number.

namespace {

struct AccentList {
    char letter;
    StringView accents;
};

// As a Mac offers them, in its order.
AccentList const s_accents[] = {
    { 'a', "\xc3\xa0 \xc3\xa1 \xc3\xa2 \xc3\xa4 \xc3\xa6 \xc3\xa3 \xc3\xa5 \xc4\x81"_sv },
    { 'A', "\xc3\x80 \xc3\x81 \xc3\x82 \xc3\x84 \xc3\x86 \xc3\x83 \xc3\x85 \xc4\x80"_sv },
    { 'c', "\xc3\xa7 \xc4\x87 \xc4\x8d"_sv },
    { 'C', "\xc3\x87 \xc4\x86 \xc4\x8c"_sv },
    { 'e', "\xc3\xa8 \xc3\xa9 \xc3\xaa \xc3\xab \xc4\x93 \xc4\x97 \xc4\x99"_sv },
    { 'E', "\xc3\x88 \xc3\x89 \xc3\x8a \xc3\x8b \xc4\x92 \xc4\x96 \xc4\x98"_sv },
    { 'i', "\xc3\xae \xc3\xaf \xc3\xad \xc4\xab \xc4\xaf \xc3\xac"_sv },
    { 'I', "\xc3\x8e \xc3\x8f \xc3\x8d \xc4\xaa \xc4\xae \xc3\x8c"_sv },
    { 'l', "\xc5\x82"_sv },
    { 'L', "\xc5\x81"_sv },
    { 'n', "\xc3\xb1 \xc5\x84"_sv },
    { 'N', "\xc3\x91 \xc5\x83"_sv },
    { 'o', "\xc3\xb4 \xc3\xb6 \xc3\xb2 \xc3\xb3 \xc5\x93 \xc3\xb8 \xc5\x8d \xc3\xb5"_sv },
    { 'O', "\xc3\x94 \xc3\x96 \xc3\x92 \xc3\x93 \xc5\x92 \xc3\x98 \xc5\x8c \xc3\x95"_sv },
    { 's', "\xc3\x9f \xc5\x9b \xc5\xa1"_sv },
    { 'S', "\xc5\x9a \xc5\xa0"_sv },
    { 'u', "\xc3\xbb \xc3\xbc \xc3\xb9 \xc3\xba \xc5\xab"_sv },
    { 'U', "\xc3\x9b \xc3\x9c \xc3\x99 \xc3\x9a \xc5\xaa"_sv },
    { 'y', "\xc3\xbf"_sv },
    { 'Y', "\xc5\xb8"_sv },
    { 'z', "\xc5\xbe \xc5\xba \xc5\xbc"_sv },
    { 'Z', "\xc5\xbd \xc5\xb9 \xc5\xbb"_sv },
};

Optional<StringView> accents_of(char letter)
{
    for (auto const& entry : s_accents) {
        if (entry.letter == letter)
            return entry.accents;
    }
    return {};
}

}

// ---- Emoji & Symbols.

class EmojiGrid final : public View {
public:
    struct Entry {
        StringView group;
        StringView emoji;
        StringView name;
    };

    static constexpr float cell = 32;
    static constexpr int columns = 10;

    explicit EmojiGrid(Function<void(StringView)> chose)
        : m_chose(move(chose))
    {
        int const fd = open("/System/Library/Input Methods/Emoji.table", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char chunk[16384];
            for (;;) {
                ssize_t const got = read(fd, chunk, sizeof(chunk));
                if (got <= 0)
                    break;
                size_t const at = m_bytes.size();
                MUST(m_bytes.try_resize(at + static_cast<size_t>(got)));
                __builtin_memcpy(m_bytes.data() + at, chunk, static_cast<size_t>(got));
            }
            close(fd);
        }
        StringView const all { m_bytes.data(), m_bytes.size() };
        size_t start = 0;
        while (start < all.length()) {
            size_t end = start;
            while (end < all.length() && all[end] != '\n')
                ++end;
            auto const line = all.substring_view(start, end - start);
            start = end + 1;
            auto const first = line.find(' ');
            if (!first.has_value())
                continue;
            auto const rest = line.substring_view(first.value() + 1);
            auto const second = rest.find(' ');
            if (!second.has_value())
                continue;
            m_all.append({ line.substring_view(0, first.value()), rest.substring_view(0, second.value()), rest.substring_view(second.value() + 1) });
        }
        filter({});
    }

    void filter(StringView search)
    {
        m_shown.clear();
        for (size_t i = 0; i < m_all.size(); ++i) {
            if (search.is_empty() || contains_folded(m_all[i].name, search))
                m_shown.append(i);
        }
        if (auto* scroll = parent())
            scroll->set_needs_layout();
        set_needs_display();
    }

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override
    {
        size_t const rows = (m_shown.size() + columns - 1) / columns;
        return { cell * columns, cell * static_cast<float>(rows) };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = Appearance::the();
        auto clip = graphics.painter().clip_bounds().to<float>();
        auto origin = graphics.painter().transform().map(Gfx::FloatPoint { 0, 0 });
        float const scale = graphics.scale();
        float const top = (clip.y - origin.y) / scale;
        float const bottom = (clip.bottom() - origin.y) / scale;
        graphics.fill({ 0, top, width(), bottom - top }, t.color("windowBackground"_sv));
        FontSpec font;
        font.size = 22;
        size_t const first_row = top <= 0 ? 0 : static_cast<size_t>(top / cell);
        for (size_t row = first_row; static_cast<float>(row) * cell < bottom; ++row) {
            for (size_t column = 0; column < columns; ++column) {
                size_t const index = row * columns + column;
                if (index >= m_shown.size())
                    return;
                graphics.draw_text_centered(m_all[m_shown[index]].emoji, font, { static_cast<float>(column) * cell, static_cast<float>(row) * cell, cell, cell }, t.color("label"_sv), TextAlignment::Center);
            }
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        size_t const column = static_cast<size_t>(event.position.x / cell);
        size_t const row = static_cast<size_t>(event.position.y / cell);
        size_t const index = row * columns + column;
        if (column < columns && index < m_shown.size())
            m_chose(m_all[m_shown[index]].emoji);
        return true;
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        // As many as could be on the screen.
        for (size_t i = 0; i < m_shown.size() && i < 120; ++i) {
            Gfx::FloatRect const rect { static_cast<float>(i % columns) * cell, static_cast<float>(i / columns) * cell, cell, cell };
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, m_all[m_shown[i]].name, rect, {}));
        }
    }

private:
    static bool contains_folded(StringView text, StringView search)
    {
        auto const fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        for (size_t at = 0; at + search.length() <= text.length(); ++at) {
            bool same = true;
            for (size_t i = 0; i < search.length() && same; ++i)
                same = fold(text[at + i]) == fold(search[i]);
            if (same)
                return true;
        }
        return false;
    }

    Function<void(StringView)> m_chose;
    Vector<char> m_bytes;
    Vector<Entry> m_all;
    Vector<size_t> m_shown;
};

class EmojiPicker final {
public:
    explicit EmojiPicker(Function<void(StringView)> chose)
        : m_window(adopt_own(*new Window("Emoji & Symbols"_sv, { 240, 160, static_cast<int>(EmojiGrid::cell * EmojiGrid::columns + 16), 420 }, 0)))
    {
        auto& stack = m_window->set_content<StackView>(Axis::Vertical, 8, Alignment::Fill, Insets { 8, 8, 8, 8 });
        auto& search = stack.add<SearchField>();
        search.set_placeholder("Search"_sv);
        auto& scroll = stack.add<ScrollView>();
        scroll.set_stretch(1, 1);
        auto& grid = scroll.make_document<EmojiGrid>(move(chose));
        search.on_change = [&grid, &search] { grid.filter(search.text()); };
        m_window->set_cancel_action([this] { m_window->close(); });
        m_window->show();
        (void)m_window->make_first_responder(&search);
    }

private:
    OwnPtr<Window> m_window;
};

InputContext& InputContext::the()
{
    static InputContext* s_the = new InputContext;
    return *s_the;
}

void InputContext::select(StringView name)
{
    m_selected_by_hand = true;
    if (m_name.view() == name)
        return;
    m_name = MUST(String::from_view(name));
    m_method = nullptr;
    if (name == "Pinyin"_sv)
        m_method = make_pinyin_input_method();
    else if (name == "Kana"_sv)
        m_method = make_kana_input_method();
    else if (name == "Hangul"_sv)
        m_method = make_hangul_input_method();
}

void InputContext::refresh()
{
    if (m_selected_by_hand)
        return;
    // Only when that file's changed.
    auto const* home = getenv("HOME");
    if (!home)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/Library/Preferences/com.vulture.desktop.json", home);
    struct stat information {};
    i64 const changed = stat(path, &information) == 0 ? static_cast<i64>(information.st_mtime) : 0;
    if (changed == m_preferences_changed)
        return;
    m_preferences_changed = changed;
    FILE* file = fopen(path, "r");
    if (!file)
        return;
    char contents[8192];
    size_t const got = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    contents[got] = 0;
    // (Only for one word, without a whole JSON parser in every program that has a text field.)
    auto const* key = strstr(contents, "\"inputMethod\"");
    if (!key)
        return;
    auto const* quote = strchr(key + 13, '"');
    if (!quote)
        return;
    auto const* end = strchr(quote + 1, '"');
    if (!end)
        return;
    select(StringView(quote + 1, static_cast<size_t>(end - quote - 1)));
    m_selected_by_hand = false;
}

void InputContext::hide_candidates()
{
    delete m_candidate_window;
    m_candidate_window = nullptr;
}

void InputContext::show_candidates(Gfx::IntPoint at)
{
    auto const candidates = m_method->candidates();
    if (candidates.is_empty()) {
        hide_candidates();
        return;
    }
    if (!m_candidate_window || m_candidate_window->room() != candidates.size()) {
        hide_candidates();
        m_candidate_window = new CandidateWindow(at, candidates.size());
    }
    m_candidate_window->list().set_candidates(candidates, m_method->selected_candidate());
}

void InputContext::open_emoji_picker(TextStorage& storage)
{
    m_emoji_target = &storage;
    // (Only one, as on a Mac: and it stays open until it's asked to go.)
    delete m_emoji_picker;
    m_emoji_picker = new EmojiPicker([this](StringView emoji) {
        if (m_emoji_target)
            m_emoji_target->replace_selection(emoji, TextStorage::EditKind::Typing);
    });
}

bool InputContext::handle_key(KeyEvent const& event, TextStorage& storage, Gfx::IntPoint caret_on_screen)
{
    refresh();

    constexpr u16 key_space = 57;
    constexpr u16 key_escape = 1;
    if (event.code == key_space && event.has_control() && event.has_command()) {
        open_emoji_picker(storage);
        return true;
    }

    // A letter's other forms, while they're showing: a number chooses, and anything else goes away and then does what it does.
    if (!m_accents.is_empty()) {
        if (event.repeat)
            return true;
        bool handled = false;
        if (event.text.length() == 1 && event.text[0] >= '1' && event.text[0] <= '9') {
            size_t const index = static_cast<size_t>(event.text[0] - '1');
            if (index < m_accents.size()) {
                // Instead of the letter that was typed to get here.
                storage.select(storage.previous_character(storage.caret()), storage.caret());
                storage.replace_selection(m_accents[index].view(), TextStorage::EditKind::Typing);
                handled = true;
            }
        }
        handled = handled || event.code == key_escape;
        m_accents.clear();
        hide_candidates();
        if (handled)
            return true;
    }

    // Held down: as a Mac has it, a letter with other forms shows them, rather than repeating.
    if (!m_method && event.repeat && event.text.length() == 1 && !event.has_command() && !event.has_control() && !event.has_option()) {
        if (auto const accents = accents_of(event.text[0]); accents.has_value()) {
            auto const list = accents.value();
            size_t start = 0;
            for (size_t i = 0; i <= list.length(); ++i) {
                if (i == list.length() || list[i] == ' ') {
                    m_accents.append(MUST(String::from_view(list.substring_view(start, i - start))));
                    start = i + 1;
                }
            }
            hide_candidates();
            m_candidate_window = new CandidateWindow(caret_on_screen, m_accents.size());
            m_candidate_window->list().set_candidates(m_accents.span(), m_accents.size());
            return true;
        }
    }

    if (!m_method)
        return false;
    auto const result = m_method->handle_key(event);
    if (!result.commit.is_empty())
        storage.commit_marked_text(result.commit.view());
    storage.set_marked_text(m_method->marked_text());
    show_candidates(caret_on_screen);
    return result.handled;
}

void InputContext::focus_lost(TextStorage& storage)
{
    if (m_emoji_target == &storage)
        m_emoji_target = nullptr;
    if (!m_accents.is_empty()) {
        m_accents.clear();
        hide_candidates();
    }
    if (!m_method)
        return;
    if (!m_method->marked_text().is_empty()) {
        auto const marked = MUST(String::from_view(m_method->marked_text()));
        storage.commit_marked_text(marked.view());
    }
    m_method->reset();
    hide_candidates();
}

// ---- Corrections, as what's typed goes in.

namespace {

// What's in the preferences file: read only when it changes, and only for a word or two, so that every program with a text field doesn't need a whole JSON parser for this.
struct PreferenceFile {
    i64 changed { -1 };
    Vector<char> contents;

    void refresh()
    {
        auto const* home = getenv("HOME");
        if (!home)
            return;
        char path[512];
        snprintf(path, sizeof(path), "%s/Library/Preferences/com.vulture.desktop.json", home);
        struct stat information {};
        i64 const now = stat(path, &information) == 0 ? static_cast<i64>(information.st_mtime) : 0;
        if (now == changed)
            return;
        changed = now;
        contents.clear();
        FILE* file = fopen(path, "r");
        if (!file)
            return;
        char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
            for (size_t i = 0; i < got; ++i)
                contents.append(chunk[i]);
        }
        fclose(file);
    }

    [[nodiscard]] bool boolean(StringView key, bool otherwise)
    {
        refresh();
        StringView const all { contents.data(), contents.size() };
        StringBuilder quoted;
        quoted.append('"');
        quoted.append(key);
        quoted.append('"');
        auto const at = all.find(quoted.view());
        if (!at.has_value())
            return otherwise;
        auto const rest = all.substring_view(at.value() + quoted.view().length());
        size_t i = 0;
        while (i < rest.length() && (rest[i] == ':' || rest[i] == ' '))
            ++i;
        return rest.substring_view(i).starts_with("true"_sv);
    }
};

PreferenceFile s_preference_file;

// What everyone gets wrong, and what a Mac's text replacements start with.
struct Replacement {
    StringView from;
    StringView to;
};

constexpr Replacement s_replacements[] = {
    { "omw"_sv, "On my way!"_sv },
};

constexpr Replacement s_corrections[] = {
    { "teh"_sv, "the"_sv }, { "adn"_sv, "and"_sv }, { "taht"_sv, "that"_sv }, { "hte"_sv, "the"_sv }, { "recieve"_sv, "receive"_sv }, { "wierd"_sv, "weird"_sv }, { "seperate"_sv, "separate"_sv },
    { "definately"_sv, "definitely"_sv }, { "occured"_sv, "occurred"_sv }, { "untill"_sv, "until"_sv }, { "becuase"_sv, "because"_sv }, { "beleive"_sv, "believe"_sv }, { "freind"_sv, "friend"_sv },
    { "dont"_sv, "don't"_sv }, { "cant"_sv, "can't"_sv }, { "wont"_sv, "won't"_sv }, { "didnt"_sv, "didn't"_sv }, { "doesnt"_sv, "doesn't"_sv }, { "isnt"_sv, "isn't"_sv }, { "im"_sv, "I'm"_sv },
};

bool is_ascii_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}

namespace {

SpellChecker& spell_checker()
{
    static SpellChecker* s_checker = [] {
        auto* checker = new SpellChecker;
        auto const read = [](char const* path) {
            Vector<char> bytes;
            FILE* file = fopen(path, "r");
            if (!file)
                return bytes;
            char chunk[65536];
            size_t got;
            while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
                size_t const at = bytes.size();
                MUST(bytes.try_resize(at + got));
                __builtin_memcpy(bytes.data() + at, chunk, got);
            }
            fclose(file);
            return bytes;
        };
        auto const dictionary = read("/usr/share/hunspell/en_US.dic");
        auto const affixes = read("/usr/share/hunspell/en_US.aff");
        if (!dictionary.is_empty())
            checker->load({ dictionary.data(), dictionary.size() }, { affixes.data(), affixes.size() });
        return checker;
    }();
    return *s_checker;
}

bool is_spelled_right(StringView word)
{
    // The same few words are asked about every time a text's drawn.
    static HashMap<String, bool> s_known;
    auto key = MUST(String::from_view(word));
    if (auto const* known = s_known.get(key))
        return *known;
    bool const right = spell_checker().is_correct(word);
    (void)s_known.try_set(move(key), right);
    return right;
}

}

void after_typing(TextStorage& storage, StringView typed)
{
    if (typed.length() != 1 || storage.has_selection() || storage.has_marked_text())
        return;
    char const c = typed[0];
    auto const text = storage.text();
    size_t const caret = storage.caret();

    // Two spaces after a word are the end of a sentence.
    if (c == ' ' && caret >= 3 && text[caret - 2] == ' ' && (is_ascii_letter(text[caret - 3]) || (text[caret - 3] >= '0' && text[caret - 3] <= '9'))
        && s_preference_file.boolean("periodWithDoubleSpace"_sv, true)) {
        storage.select(caret - 2, caret);
        storage.replace_selection(". "_sv);
        return;
    }

    // The first letter of a sentence.
    if (c >= 'a' && c <= 'z' && s_preference_file.boolean("capitalizeWords"_sv, true)) {
        size_t const before = caret - 1;
        bool sentence_starts = before == 0;
        if (before > 0 && text[before - 1] == '\n') {
            sentence_starts = true;
        } else if (before >= 2 && text[before - 1] == ' ') {
            // However many spaces.
            size_t back = before;
            while (back > 0 && text[back - 1] == ' ')
                --back;
            sentence_starts = back > 0 && (text[back - 1] == '.' || text[back - 1] == '!' || text[back - 1] == '?');
        }
        if (sentence_starts) {
            char const upper[2] = { static_cast<char>(c - 32), 0 };
            storage.select(before, caret);
            storage.replace_selection(StringView(upper, 1), TextStorage::EditKind::Typing);
            return;
        }
    }

    // A word that's just been finished.
    bool const ends_word = c == ' ' || c == '\n' || c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':';
    if (!ends_word || caret < 2)
        return;
    size_t const end = caret - 1;
    size_t start = end;
    while (start > 0 && is_ascii_letter(text[start - 1]))
        --start;
    if (start == end)
        return;
    auto const word = text.substring_view(start, end - start);
    auto const apply = [&](StringView replacement) {
        StringBuilder out;
        out.append(replacement);
        out.append(c);
        auto const result = MUST(out.to_string());
        storage.select(start, caret);
        storage.replace_selection(result.view());
    };
    if (s_preference_file.boolean("useTextReplacements"_sv, true)) {
        for (auto const& replacement : s_replacements) {
            if (word == replacement.from) {
                apply(replacement.to);
                return;
            }
        }
    }
    if (s_preference_file.boolean("correctSpelling"_sv, true)) {
        // Whether or not it's been capitalized by now, and it stays that way.
        bool const capitalized = word[0] >= 'A' && word[0] <= 'Z';
        for (auto const& correction : s_corrections) {
            if (word.length() != correction.from.length())
                continue;
            bool same = true;
            for (size_t i = 0; i < word.length() && same; ++i) {
                char const letter = i == 0 && capitalized ? static_cast<char>(word[i] + 32) : word[i];
                same = letter == correction.from[i];
            }
            if (same) {
                StringBuilder corrected;
                corrected.append(capitalized && correction.to[0] >= 'a' && correction.to[0] <= 'z' ? static_cast<char>(correction.to[0] - 32) : correction.to[0]);
                corrected.append(correction.to.substring_view(1));
                apply(MUST(corrected.to_string()).view());
                return;
            }
        }
        // Two letters the wrong way round, and nothing else it could be: that much it's safe to fix without asking.
        if (word.length() >= 4 && !spell_checker().is_correct(word)) {
            auto const suggestions = spell_checker().suggestions(word, 1);
            if (!suggestions.is_empty() && suggestions[0].view().length() == word.length()) {
                size_t different = 0;
                for (size_t i = 0; i < word.length(); ++i) {
                    char const a = word[i] >= 'A' && word[i] <= 'Z' ? static_cast<char>(word[i] + 32) : word[i];
                    if (a != suggestions[0].view()[i])
                        ++different;
                }
                if (different == 2) {
                    StringBuilder corrected;
                    corrected.append(capitalized ? static_cast<char>(suggestions[0].view()[0] - 32) : suggestions[0].view()[0]);
                    corrected.append(suggestions[0].view().substring_view(1));
                    apply(MUST(corrected.to_string()).view());
                }
            }
        }
    }
}

void install_input_context()
{
    g_text_input_hooks.show_emoji = [](TextStorage& storage) { InputContext::the().open_emoji_picker(storage); };
    g_text_input_hooks.typed = after_typing;
    g_text_input_hooks.is_spelled_right = is_spelled_right;
    g_text_input_hooks.key = [](KeyEvent const& event, TextStorage& storage, Gfx::IntPoint caret) { return InputContext::the().handle_key(event, storage, caret); };
    g_text_input_hooks.focus_lost = [](TextStorage& storage) { InputContext::the().focus_lost(storage); };
}

}
