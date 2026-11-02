/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// The screen of an xterm-like terminal: UTF-8 text, the C0 controls, ESC and CSI
// sequences, colors (16, 256 and 24-bit), scroll regions, the alternate screen, and
// scrollback. Bytes go in; cells, a cursor and a title come out.

namespace VT {

// A color: the default, one of the 256, or 24-bit.
struct Color {
    enum class Kind : u8 {
        Default,
        Indexed,
        RGB,
    };
    Kind kind { Kind::Default };
    u8 index { 0 };
    u8 red { 0 };
    u8 green { 0 };
    u8 blue { 0 };

    bool operator==(Color const&) const = default;
};

struct Attributes {
    Color foreground;
    Color background;
    bool bold { false };
    bool faint { false };
    bool italic { false };
    bool underline { false };
    bool inverse { false };
    bool strikethrough { false };

    bool operator==(Attributes const&) const = default;
};

struct Cell {
    u32 codepoint { ' ' };
    Attributes attributes;
};

class Terminal {
public:
    Terminal(int columns, int rows, int scrollback_lines = 2000);

    void feed(ReadonlyBytes);
    void feed(StringView text) { feed(ReadonlyBytes(reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length())); }
    // Lines keep what they have, cut or padded; the screen's bottom stays at the bottom.
    void resize(int columns, int rows);

    [[nodiscard]] int columns() const { return m_columns; }
    [[nodiscard]] int rows() const { return m_rows; }
    [[nodiscard]] int cursor_row() const { return m_cursor_row; }
    [[nodiscard]] int cursor_column() const { return m_cursor_column; }
    [[nodiscard]] bool cursor_visible() const { return m_cursor_visible; }
    [[nodiscard]] Cell const& cell(int row, int column) const { return screen()[row][column]; }
    [[nodiscard]] Vector<Cell> const& screen_line(int row) const { return screen()[row]; }
    [[nodiscard]] StringView title() const { return m_title.view(); }

    // Lines gone off the top, oldest first (on the main screen only).
    [[nodiscard]] size_t scrollback_size() const { return m_scrollback.size(); }
    [[nodiscard]] Vector<Cell> const& scrollback_line(size_t index) const { return m_scrollback[index]; }

    // What the keys should send, as the program has asked.
    [[nodiscard]] bool application_cursor_keys() const { return m_application_cursor_keys; }
    [[nodiscard]] bool bracketed_paste() const { return m_bracketed_paste; }
    [[nodiscard]] bool using_alternate_screen() const { return m_alternate; }

    // The text of a row, trailing blanks gone (for selection, and tests).
    [[nodiscard]] String row_text(int row) const;

    // Answers to what the program asked (cursor position, who are you).
    Function<void(StringView)> on_reply;
    Function<void()> on_bell;
    Function<void()> on_title_change;

private:
    using Line = Vector<Cell>;

    [[nodiscard]] Vector<Line> const& screen() const { return m_alternate ? m_alternate_screen : m_main_screen; }
    [[nodiscard]] Vector<Line>& screen() { return m_alternate ? m_alternate_screen : m_main_screen; }
    [[nodiscard]] Line blank_line() const;

    void put_codepoint(u32);
    void new_line();
    void reverse_line();
    void scroll_up(int count);
    void scroll_down(int count);
    void erase_in_display(int mode);
    void erase_in_line(int mode);
    void insert_lines(int count);
    void delete_lines(int count);
    void insert_characters(int count);
    void delete_characters(int count);
    void erase_characters(int count);
    void set_cursor(int row, int column);
    void set_alternate(bool);
    void reset();

    void execute_control(u8);
    void handle_escape(u8);
    void handle_csi(u8 final);
    void handle_osc();
    void select_graphic_rendition();
    void set_private_mode(int mode, bool on);
    [[nodiscard]] int parameter(size_t index, int fallback) const;

    enum class State : u8 {
        Ground,
        Escape,
        EscapeIntermediate,
        CSI,
        OSC,
        OSCEscape,
    };

    int m_columns;
    int m_rows;
    int m_scrollback_limit;
    Vector<Line> m_main_screen;
    Vector<Line> m_alternate_screen;
    Vector<Line> m_scrollback;
    bool m_alternate { false };

    int m_cursor_row { 0 };
    int m_cursor_column { 0 };
    // At the last column, after a character: the next one wraps first.
    bool m_wrap_pending { false };
    bool m_auto_wrap { true };
    bool m_cursor_visible { true };
    bool m_application_cursor_keys { false };
    bool m_bracketed_paste { false };
    bool m_origin_mode { false };
    int m_scroll_top { 0 };
    int m_scroll_bottom { 0 };
    Attributes m_attributes;
    int m_saved_row { 0 };
    int m_saved_column { 0 };
    Attributes m_saved_attributes;
    String m_title;

    State m_state { State::Ground };
    Vector<int> m_parameters;
    int m_current_parameter { -1 };
    bool m_private_marker { false };
    Vector<char> m_osc;

    u32 m_utf8_codepoint { 0 };
    int m_utf8_remaining { 0 };
};

}
