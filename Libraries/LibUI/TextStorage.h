/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibUI/Event.h>

// Text being edited, with what a field and a text view share: the caret and selection,
// moving by character and word, and undo. Offsets are in bytes of UTF-8, always at
// the start of a character.

namespace UI {

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
    };

    void apply(size_t position, size_t removed_length, StringView inserted);
    void changed();

    Vector<char> m_text;
    size_t m_caret { 0 };
    size_t m_anchor { 0 };
    Vector<Edit> m_undo;
    Vector<Edit> m_redo;
    EditKind m_last_kind { EditKind::Other };
};

}
