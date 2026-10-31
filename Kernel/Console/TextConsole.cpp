/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Console/Font.h>
#include <Kernel/Console/TextConsole.h>

namespace Kernel {

namespace {

constexpr u8 default_color = 16;

// The 16 ANSI colors, as macOS Terminal's Basic profile shows them.
constexpr Color s_palette[16] = {
    { 0x00, 0x00, 0x00 },
    { 0x99, 0x00, 0x00 },
    { 0x00, 0xa6, 0x00 },
    { 0x99, 0x99, 0x00 },
    { 0x00, 0x00, 0xb2 },
    { 0xb2, 0x00, 0xb2 },
    { 0x00, 0xa6, 0xb2 },
    { 0xbf, 0xbf, 0xbf },
    { 0x66, 0x66, 0x66 },
    { 0xe5, 0x00, 0x00 },
    { 0x00, 0xd9, 0x00 },
    { 0xe5, 0xe5, 0x00 },
    { 0x00, 0x00, 0xff },
    { 0xe5, 0x00, 0xe5 },
    { 0x00, 0xe5, 0xe5 },
    { 0xe5, 0xe5, 0xe5 },
};

}

void TextConsole::initialize(Framebuffer& framebuffer)
{
    m_framebuffer = &framebuffer;
    // Keep text readable on high-resolution screens.
    m_scale = framebuffer.width() >= 2048 ? 2 : 1;
    m_margin = 8 * m_scale;
    m_columns = min<u32>((framebuffer.width() - 2 * m_margin) / (Font::glyph_width * m_scale), max_columns);
    m_rows = min<u32>((framebuffer.height() - 2 * m_margin) / (Font::glyph_height * m_scale), max_rows);
    m_attributes = {};
    m_attributes.foreground = default_color;
    m_state = State::Normal;
    m_cursor_visible = true;
    m_cursor_drawn = false;
    clear();
}

void TextConsole::set_colors(Color foreground, Color background)
{
    m_foreground = foreground;
    m_background = background;
}

Color TextConsole::color(u8 index, bool foreground, bool bold) const
{
    if (index == default_color)
        return foreground ? m_foreground : m_background;
    // Bold brightens the eight basic colors.
    if (foreground && bold && index < 8)
        index += 8;
    return s_palette[index];
}

void TextConsole::clear()
{
    if (!m_framebuffer)
        return;
    m_framebuffer->fill_rect(0, 0, static_cast<i32>(m_framebuffer->width()), static_cast<i32>(m_framebuffer->height()), m_background);
    for (u32 row = 0; row < m_rows; ++row) {
        for (u32 column = 0; column < m_columns; ++column)
            m_cells[row][column] = { ' ', default_color, default_color, false };
    }
    m_column = 0;
    m_row = 0;
    m_pending_wrap = false;
    m_cursor_drawn = false;
}

void TextConsole::write(StringView text)
{
    if (!m_framebuffer)
        return;
    show_cursor(false);
    for (char character : text)
        put(character);
    show_cursor(true);
}

void TextConsole::draw_cell(u32 column, u32 row, bool inverted)
{
    auto const& cell = m_cells[row][column];
    Color foreground = color(cell.foreground, true, cell.bold);
    Color background = color(cell.background, false, false);
    if (inverted) {
        Color swap = foreground;
        foreground = background;
        background = swap;
    }
    u8 const* glyph = Font::glyph(cell.character);
    i32 origin_x = static_cast<i32>(m_margin + column * Font::glyph_width * m_scale);
    i32 origin_y = static_cast<i32>(m_margin + row * Font::glyph_height * m_scale);
    i32 scale = static_cast<i32>(m_scale);
    for (u32 y = 0; y < Font::glyph_height; ++y) {
        for (u32 x = 0; x < Font::glyph_width; ++x) {
            bool lit = glyph[y] & (0x80 >> x);
            m_framebuffer->fill_rect(origin_x + static_cast<i32>(x) * scale, origin_y + static_cast<i32>(y) * scale, scale, scale, lit ? foreground : background);
        }
    }
}

// The cursor is the cell under it drawn inverted.
void TextConsole::show_cursor(bool show)
{
    u32 column = min(m_column, m_columns - 1);
    if (show && m_cursor_visible && !m_cursor_drawn) {
        draw_cell(column, m_row, true);
        m_cursor_drawn = true;
    } else if (!show && m_cursor_drawn) {
        draw_cell(column, m_row, false);
        m_cursor_drawn = false;
    }
}

void TextConsole::scroll()
{
    m_framebuffer->scroll_up(Font::glyph_height * m_scale, m_background);
    for (u32 row = 1; row < m_rows; ++row) {
        for (u32 column = 0; column < m_columns; ++column)
            m_cells[row - 1][column] = m_cells[row][column];
    }
    for (u32 column = 0; column < m_columns; ++column)
        m_cells[m_rows - 1][column] = { ' ', default_color, m_attributes.background, false };
    // The new line takes the current background, as terminals do.
    if (m_attributes.background != default_color)
        erase(0, m_rows - 1, m_columns - 1, m_rows - 1);
}

void TextConsole::new_line()
{
    m_pending_wrap = false;
    if (m_row + 1 < m_rows)
        ++m_row;
    else
        scroll();
}

void TextConsole::erase(u32 from_column, u32 from_row, u32 to_column, u32 to_row)
{
    for (u32 row = from_row; row <= to_row && row < m_rows; ++row) {
        u32 first = row == from_row ? from_column : 0;
        u32 last = row == to_row ? to_column : m_columns - 1;
        for (u32 column = first; column <= last && column < m_columns; ++column) {
            m_cells[row][column] = { ' ', default_color, m_attributes.background, false };
            draw_cell(column, row, false);
        }
    }
}

void TextConsole::print(char character)
{
    if (m_pending_wrap) {
        m_column = 0;
        new_line();
    }
    m_cells[m_row][m_column] = { character, m_attributes.foreground, m_attributes.background, m_attributes.bold };
    draw_cell(m_column, m_row, false);
    if (m_column + 1 < m_columns)
        ++m_column;
    else
        m_pending_wrap = true;
}

u32 TextConsole::parameter(size_t index, u32 fallback) const
{
    if (index >= m_parameter_count || m_parameters[index] == 0)
        return fallback;
    return m_parameters[index];
}

void TextConsole::set_attributes()
{
    if (m_parameter_count == 0) {
        m_attributes.foreground = default_color;
        m_attributes.background = default_color;
        m_attributes.bold = false;
        return;
    }
    for (size_t i = 0; i < m_parameter_count; ++i) {
        u32 value = m_parameters[i];
        if (value == 0) {
            m_attributes.foreground = default_color;
            m_attributes.background = default_color;
            m_attributes.bold = false;
        } else if (value == 1) {
            m_attributes.bold = true;
        } else if (value == 22) {
            m_attributes.bold = false;
        } else if (value >= 30 && value <= 37) {
            m_attributes.foreground = static_cast<u8>(value - 30);
        } else if (value == 39) {
            m_attributes.foreground = default_color;
        } else if (value >= 40 && value <= 47) {
            m_attributes.background = static_cast<u8>(value - 40);
        } else if (value == 49) {
            m_attributes.background = default_color;
        } else if (value >= 90 && value <= 97) {
            m_attributes.foreground = static_cast<u8>(value - 90 + 8);
        } else if (value >= 100 && value <= 107) {
            m_attributes.background = static_cast<u8>(value - 100 + 8);
        }
    }
}

void TextConsole::execute_control_sequence(char final)
{
    m_pending_wrap = false;
    switch (final) {
    case 'A':
        m_row -= min(parameter(0, 1), m_row);
        break;
    case 'B':
        m_row = min(m_row + parameter(0, 1), m_rows - 1);
        break;
    case 'C':
        m_column = min(m_column + parameter(0, 1), m_columns - 1);
        break;
    case 'D':
        m_column -= min(parameter(0, 1), m_column);
        break;
    case 'E':
        m_row = min(m_row + parameter(0, 1), m_rows - 1);
        m_column = 0;
        break;
    case 'F':
        m_row -= min(parameter(0, 1), m_row);
        m_column = 0;
        break;
    case 'G':
        m_column = min(parameter(0, 1), m_columns) - 1;
        break;
    case 'd':
        m_row = min(parameter(0, 1), m_rows) - 1;
        break;
    case 'H':
    case 'f':
        m_row = min(parameter(0, 1), m_rows) - 1;
        m_column = min(parameter(1, 1), m_columns) - 1;
        break;
    case 'J': {
        u32 mode = m_parameter_count > 0 ? m_parameters[0] : 0;
        if (mode == 0)
            erase(m_column, m_row, m_columns - 1, m_rows - 1);
        else if (mode == 1)
            erase(0, 0, m_column, m_row);
        else
            erase(0, 0, m_columns - 1, m_rows - 1);
        break;
    }
    case 'K': {
        u32 mode = m_parameter_count > 0 ? m_parameters[0] : 0;
        if (mode == 0)
            erase(m_column, m_row, m_columns - 1, m_row);
        else if (mode == 1)
            erase(0, m_row, m_column, m_row);
        else
            erase(0, m_row, m_columns - 1, m_row);
        break;
    }
    case 'm':
        set_attributes();
        break;
    case 's':
        m_saved_column = m_column;
        m_saved_row = m_row;
        break;
    case 'u':
        m_column = m_saved_column;
        m_row = m_saved_row;
        break;
    case 'h':
    case 'l':
        if (m_private && parameter(0, 0) == 25)
            m_cursor_visible = final == 'h';
        break;
    default:
        // Anything else is ignored, as a terminal would.
        break;
    }
}

void TextConsole::put(char character)
{
    switch (m_state) {
    case State::Escape:
        if (character == '[') {
            m_state = State::ControlSequence;
            m_parameter_count = 0;
            m_private = false;
            for (auto& value : m_parameters)
                value = 0;
            return;
        }
        if (character == '7') {
            m_saved_column = m_column;
            m_saved_row = m_row;
        } else if (character == '8') {
            m_column = m_saved_column;
            m_row = m_saved_row;
        } else if (character == 'c') {
            m_attributes = {};
            m_attributes.foreground = default_color;
            clear();
        }
        m_state = State::Normal;
        return;
    case State::ControlSequence:
        if (character >= '0' && character <= '9') {
            if (m_parameter_count == 0)
                m_parameter_count = 1;
            auto& value = m_parameters[m_parameter_count - 1];
            value = min<u32>(value * 10 + static_cast<u32>(character - '0'), 9999);
            return;
        }
        if (character == ';') {
            if (m_parameter_count == 0)
                m_parameter_count = 1;
            if (m_parameter_count < max_parameters)
                ++m_parameter_count;
            return;
        }
        if (character == '?') {
            m_private = true;
            return;
        }
        if (character >= 0x40 && character <= 0x7e) {
            execute_control_sequence(character);
            m_state = State::Normal;
        }
        return;
    case State::Normal:
        break;
    }

    switch (character) {
    case '\x1b':
        m_state = State::Escape;
        return;
    case '\n':
        new_line();
        return;
    case '\r':
        m_column = 0;
        m_pending_wrap = false;
        return;
    case '\b':
        if (m_column > 0 && !m_pending_wrap)
            --m_column;
        m_pending_wrap = false;
        return;
    case '\t':
        do {
            print(' ');
        } while (m_column % 8 != 0 && !m_pending_wrap);
        return;
    case '\a':
        return;
    default:
        if (static_cast<u8>(character) < 0x20)
            return;
        print(character);
    }
}

TextConsole& boot_text_console()
{
    static TextConsole console;
    return console;
}

}
