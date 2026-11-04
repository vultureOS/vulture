/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibGfx/Geometry.h>
#include <LibBase/Function.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibUI/Event.h>

// Text being edited, with what a field and a text view share: the caret and selection,
// moving by character and word, and undo. Offsets are in bytes of UTF-8, always at
// the start of a character.

namespace UI {

class TextStorage;
struct KeyEvent;

// How part of a text looks, in a text view that's rich.
struct TextAttributes {
    bool bold { false };
    bool italic { false };
    bool underline { false };
    bool strikethrough { false };
    // In points: 0 for the view's own.
    float size { 0 };
    bool has_color { false };
    Gfx::Color color {};

    bool operator==(TextAttributes const&) const = default;
};

struct AttributeRun {
    size_t start { 0 };
    size_t end { 0 };
    TextAttributes attributes;
};

// Who gets to look at each key before a text control does: an input method, if there's a system to have one on. (The toolkit's built for tests with no system at all.)
struct TextInputHooks {
    bool (*key)(KeyEvent const&, TextStorage&, Gfx::IntPoint caret_on_screen) { nullptr };
    void (*focus_lost)(TextStorage&) { nullptr };
    void (*show_emoji)(TextStorage&) { nullptr };
    // After something's been typed, in a view of running text: for corrections and capitals.
    void (*typed)(TextStorage&, StringView) { nullptr };
    // Whether a word's spelled right, if there's a dictionary to ask.
    bool (*is_spelled_right)(StringView) { nullptr };
};
extern TextInputHooks g_text_input_hooks;

class TextStorage {
public:
    [[nodiscard]] StringView text() const { return StringView(m_text.data(), m_text.size()); }
    [[nodiscard]] size_t length() const { return m_text.size(); }

    // Replaces everything, and forgets what could be undone.
    void set_text(StringView);

    [[nodiscard]] size_t caret() const { return m_caret; }
    [[nodiscard]] size_t anchor() const { return m_anchor; }
    [[nodiscard]] bool has_selection() const { return m_caret != m_anchor; }
    [[nodiscard]] size_t selection_start() const { return min(m_caret, m_anchor); }
    [[nodiscard]] size_t selection_end() const { return max(m_caret, m_anchor); }
    [[nodiscard]] StringView selected_text() const { return text().substring_view(selection_start(), selection_end() - selection_start()); }
    void select(size_t anchor, size_t caret);
    void select_all() { select(0, m_text.size()); }
    // Moves the caret; the selection stretches with it, or goes.
    void move_caret(size_t to, bool extend);

    [[nodiscard]] size_t next_character(size_t) const;
    [[nodiscard]] size_t previous_character(size_t) const;
    [[nodiscard]] size_t next_word(size_t) const;
    [[nodiscard]] size_t previous_word(size_t) const;
    [[nodiscard]] size_t line_start(size_t) const;
    [[nodiscard]] size_t line_end(size_t) const;
    // The word around this offset.
    void word_at(size_t, size_t& start, size_t& end) const;

    enum class EditKind : u8 {
        // Runs of it undo together.
        Typing,
        Deleting,
        Other,
    };
    // What is selected gives way to this.
    void replace_selection(StringView, EditKind = EditKind::Other);
    void delete_backward(size_t to);
    void delete_forward(size_t to);

    // ---- Rich text. Runs cover the whole text, with what's alike joined up. What's typed takes after what's before it, or what's been asked for since the caret was last moved.
    [[nodiscard]] bool is_rich() const { return m_rich; }
    void set_rich(bool);
    [[nodiscard]] Span<AttributeRun const> attribute_runs() const { return m_runs.span(); }
    [[nodiscard]] TextAttributes attributes_at(size_t offset) const;
    // For the selection, if there is one, with an undo; if not, for whatever's typed next. `change` is given what's there now.
    void change_attributes(Function<void(TextAttributes&)> change);
    // Of the whole selection, or of what would be typed: for a menu to show a check mark.
    [[nodiscard]] TextAttributes typing_attributes() const;

    // ---- What an input method is still working on: shown in the text, underlined, but not part of it until it's been decided. Nothing can undo it, as it hasn't happened yet.
    [[nodiscard]] bool has_marked_text() const { return m_marked_length != 0; }
    [[nodiscard]] size_t marked_start() const { return m_marked_start; }
    [[nodiscard]] size_t marked_end() const { return m_marked_start + m_marked_length; }
    void set_marked_text(StringView);
    // Instead of what was marked, or at the caret if nothing was, and with an undo like typing.
    void commit_marked_text(StringView);

    [[nodiscard]] bool can_undo() const { return !m_undo.is_empty(); }
    [[nodiscard]] bool can_redo() const { return !m_redo.is_empty(); }
    void undo();
    void redo();
    // The next edit starts a new step, as moving the caret does.
    void end_undo_group() { m_last_kind = EditKind::Other; }

    Function<void()> on_change;

private:
    struct Edit {
        size_t position;
        String removed;
        String inserted;
        size_t caret_before;
        size_t anchor_before;
        EditKind kind;
        Vector<AttributeRun> runs_before {};
        Vector<AttributeRun> runs_after {};
    };

    void apply(size_t position, size_t removed_length, StringView inserted);
    void join_runs();
    void split_run_at(size_t);
    void changed();

    Vector<char> m_text;
    bool m_rich { false };
    Vector<AttributeRun> m_runs;
    Optional<TextAttributes> m_pending_attributes;
    size_t m_marked_start { 0 };
    size_t m_marked_length { 0 };
    size_t m_caret { 0 };
    size_t m_anchor { 0 };
    Vector<Edit> m_undo;
    Vector<Edit> m_redo;
    EditKind m_last_kind { EditKind::Other };
};

}
