/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Console/Framebuffer.h>
#include <LibBase/StringView.h>

namespace Kernel {

// A text console drawn with the built-in font: the verbose boot log ("-v"),
// the panic report, and /dev/console's screen once userland runs.
//
// It understands the ANSI/VT100 sequences programs use: cursor movement and
// positioning (CSI A B C D E F G H f d), erasing (J, K), 16-color and bold
// attributes (m), save and restore (s u, ESC 7 / 8), showing and hiding the
// cursor (?25h / ?25l), plus backspace, tab, CR and LF.
class TextConsole {
public:
    void initialize(Framebuffer&);

    [[nodiscard]] bool is_ready() const { return m_framebuffer != nullptr; }
    [[nodiscard]] u32 columns() const { return m_columns; }
    [[nodiscard]] u32 rows() const { return m_rows; }

    void set_colors(Color foreground, Color background);
    void clear();
    void write(StringView);

private:
    static constexpr u32 max_columns = 320;
    static constexpr u32 max_rows = 120;
    static constexpr size_t max_parameters = 8;

    struct Cell {
        char character { ' ' };
        u8 foreground { 7 }; // Palette indices; 16 means the default.
        u8 background { 16 };
        bool bold { false };
    };

    enum class State : u8 {
        Normal,
        Escape,
        ControlSequence,
    };

    void put(char);
    void print(char);
    void new_line();
    void scroll();
    void draw_cell(u32 column, u32 row, bool inverted);
    void show_cursor(bool);
    void erase(u32 from_column, u32 from_row, u32 to_column, u32 to_row);
    void execute_control_sequence(char final);
    void set_attributes();
    [[nodiscard]] Color color(u8 index, bool foreground, bool bold) const;
    [[nodiscard]] u32 parameter(size_t index, u32 fallback) const;

    Framebuffer* m_framebuffer { nullptr };
    u32 m_scale { 1 };
    u32 m_margin { 0 };
    u32 m_columns { 0 };
    u32 m_rows { 0 };
    u32 m_column { 0 };
    u32 m_row { 0 };
    u32 m_saved_column { 0 };
    u32 m_saved_row { 0 };
    bool m_cursor_visible { false };
    bool m_cursor_drawn { false };
    // A full line wraps only when the next character comes.
    bool m_pending_wrap { false };

    State m_state { State::Normal };
    u32 m_parameters[max_parameters] {};
    size_t m_parameter_count { 0 };
    bool m_private { false };

    Cell m_attributes;
    Color m_foreground { 0xd8, 0xd8, 0xd8 };
    Color m_background { 0x00, 0x00, 0x00 };
    Cell m_cells[max_rows][max_columns];
};

TextConsole& boot_text_console();

}
