/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Console/Font.h>
#include <Kernel/Console/TextConsole.h>

namespace Kernel {

void TextConsole::initialize(Framebuffer& framebuffer)
{
    m_framebuffer = &framebuffer;
    // Keep text readable on high-resolution screens.
    m_scale = framebuffer.width() >= 2048 ? 2 : 1;
    m_margin = 8 * m_scale;
    m_columns = (framebuffer.width() - 2 * m_margin) / (Font::glyph_width * m_scale);
    m_rows = (framebuffer.height() - 2 * m_margin) / (Font::glyph_height * m_scale);
    clear();
}

void TextConsole::set_colors(Color foreground, Color background)
{
    m_foreground = foreground;
    m_background = background;
}

void TextConsole::clear()
{
    if (!m_framebuffer)
        return;
    m_framebuffer->fill_rect(0, 0, static_cast<i32>(m_framebuffer->width()), static_cast<i32>(m_framebuffer->height()), m_background);
    m_column = 0;
    m_row = 0;
}

void TextConsole::write(StringView text)
{
    if (!m_framebuffer)
        return;
    for (char character : text)
        put(character);
}

void TextConsole::put(char character)
{
    switch (character) {
    case '\n':
        new_line();
        return;
    case '\r':
        m_column = 0;
        return;
    case '\t':
        do {
            put(' ');
        } while (m_column % 4 != 0);
        return;
    default:
        break;
    }
    if (m_column == m_columns)
        new_line();
    draw(character, m_column, m_row);
    ++m_column;
}

void TextConsole::new_line()
{
    m_column = 0;
    if (m_row + 1 < m_rows) {
        ++m_row;
        return;
    }
    m_framebuffer->scroll_up(Font::glyph_height * m_scale, m_background);
}

void TextConsole::draw(char character, u32 column, u32 row)
{
    u8 const* glyph = Font::glyph(character);
    i32 origin_x = static_cast<i32>(m_margin + column * Font::glyph_width * m_scale);
    i32 origin_y = static_cast<i32>(m_margin + row * Font::glyph_height * m_scale);
    i32 scale = static_cast<i32>(m_scale);
    for (u32 y = 0; y < Font::glyph_height; ++y) {
        for (u32 x = 0; x < Font::glyph_width; ++x) {
            bool lit = glyph[y] & (0x80 >> x);
            m_framebuffer->fill_rect(origin_x + static_cast<i32>(x) * scale, origin_y + static_cast<i32>(y) * scale, scale, scale, lit ? m_foreground : m_background);
        }
    }
}

TextConsole& boot_text_console()
{
    static TextConsole console;
    return console;
}

}
