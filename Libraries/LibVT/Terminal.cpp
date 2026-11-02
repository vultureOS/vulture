/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibVT/Terminal.h>

namespace VT {

Terminal::Terminal(int columns, int rows, int scrollback_lines)
    : m_columns(max(columns, 1))
    , m_rows(max(rows, 1))
    , m_scrollback_limit(scrollback_lines)
{
    for (int i = 0; i < m_rows; ++i) {
        m_main_screen.append(blank_line());
        m_alternate_screen.append(blank_line());
    }
    m_scroll_bottom = m_rows - 1;
}

Terminal::Line Terminal::blank_line() const
{
    Line line;
    Cell blank;
    blank.attributes.background = m_attributes.background;
    MUST(line.try_resize(static_cast<size_t>(m_columns)));
    for (auto& cell : line)
        cell = blank;
    return line;
}

void Terminal::reset()
{
    m_attributes = {};
    m_cursor_row = m_cursor_column = 0;
    m_wrap_pending = false;
    m_auto_wrap = true;
    m_cursor_visible = true;
    m_application_cursor_keys = false;
    m_bracketed_paste = false;
    m_origin_mode = false;
    m_scroll_top = 0;
    m_scroll_bottom = m_rows - 1;
    for (auto& line : screen())
        line = blank_line();
}

void Terminal::resize(int columns, int rows)
{
    columns = max(columns, 1);
    rows = max(rows, 1);
    if (columns == m_columns && rows == m_rows)
        return;
    auto fit_line = [&](Line& line) {
        size_t old_size = line.size();
        MUST(line.try_resize(static_cast<size_t>(columns)));
        for (size_t i = old_size; i < static_cast<size_t>(columns); ++i)
            line[i] = Cell {};
    };
    auto fit_screen = [&](Vector<Line>& lines, bool keep_in_scrollback) {
        for (auto& line : lines)
            fit_line(line);
        // Fewer rows: from the top, into the scrollback, while the cursor stays on.
        while (static_cast<int>(lines.size()) > rows) {
            if (keep_in_scrollback && m_cursor_row >= rows) {
                m_scrollback.append(move(lines[0]));
                lines.remove(0);
                --m_cursor_row;
            } else {
                (void)lines.take_last();
            }
        }
        while (static_cast<int>(lines.size()) < rows) {
            Line blank;
            MUST(blank.try_resize(static_cast<size_t>(columns)));
            for (auto& cell : blank)
                cell = Cell {};
            lines.append(move(blank));
        }
    };
    fit_screen(m_main_screen, !m_alternate);
    fit_screen(m_alternate_screen, false);
    m_columns = columns;
    m_rows = rows;
    m_scroll_top = 0;
    m_scroll_bottom = rows - 1;
    m_cursor_row = clamp(m_cursor_row, 0, rows - 1);
    m_cursor_column = clamp(m_cursor_column, 0, columns - 1);
    m_wrap_pending = false;
}

String Terminal::row_text(int row) const
{
    StringBuilder builder;
    auto const& line = screen()[row];
    int last = m_columns - 1;
    while (last >= 0 && line[last].codepoint == ' ')
        --last;
    for (int i = 0; i <= last; ++i) {
        u32 c = line[i].codepoint;
        char encoded[4];
        size_t length = 0;
        if (c < 0x80) {
            encoded[length++] = static_cast<char>(c);
        } else if (c < 0x800) {
            encoded[length++] = static_cast<char>(0xC0 | (c >> 6));
            encoded[length++] = static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            encoded[length++] = static_cast<char>(0xE0 | (c >> 12));
            encoded[length++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            encoded[length++] = static_cast<char>(0x80 | (c & 0x3F));
        } else {
            encoded[length++] = static_cast<char>(0xF0 | (c >> 18));
            encoded[length++] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            encoded[length++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            encoded[length++] = static_cast<char>(0x80 | (c & 0x3F));
        }
        builder.append(StringView(encoded, length));
    }
    return MUST(builder.to_string());
}

void Terminal::feed(ReadonlyBytes bytes)
{
    for (size_t i = 0; i < bytes.size(); ++i) {
        u8 byte = bytes[i];
        switch (m_state) {
        case State::Ground:
            if (m_utf8_remaining > 0) {
                if ((byte & 0xC0) == 0x80) {
                    m_utf8_codepoint = (m_utf8_codepoint << 6) | (byte & 0x3F);
                    if (--m_utf8_remaining == 0)
                        put_codepoint(m_utf8_codepoint);
                    continue;
                }
                // Cut short: what there was stands for one bad character.
                m_utf8_remaining = 0;
                put_codepoint(0xFFFD);
            }
            if (byte < 0x20 || byte == 0x7F) {
                execute_control(byte);
            } else if (byte < 0x80) {
                put_codepoint(byte);
            } else if ((byte & 0xE0) == 0xC0) {
                m_utf8_codepoint = byte & 0x1F;
                m_utf8_remaining = 1;
            } else if ((byte & 0xF0) == 0xE0) {
                m_utf8_codepoint = byte & 0x0F;
                m_utf8_remaining = 2;
            } else if ((byte & 0xF8) == 0xF0) {
                m_utf8_codepoint = byte & 0x07;
                m_utf8_remaining = 3;
            } else {
                put_codepoint(0xFFFD);
            }
            break;
        case State::Escape:
            handle_escape(byte);
            break;
        case State::EscapeIntermediate:
            // Character sets: there is only the one.
            m_state = State::Ground;
            break;
        case State::CSI:
            if (byte >= '0' && byte <= '9') {
                if (m_current_parameter < 0)
                    m_current_parameter = 0;
                // (Bounded: nothing here means anything past this.)
                if (m_current_parameter < 100000)
                    m_current_parameter = m_current_parameter * 10 + (byte - '0');
            } else if (byte == ';' || byte == ':') {
                m_parameters.append(m_current_parameter);
                m_current_parameter = -1;
            } else if (byte == '?' || byte == '>' || byte == '=' || byte == '<') {
                m_private_marker = true;
            } else if (byte >= 0x40 && byte <= 0x7E) {
                if (m_current_parameter >= 0 || !m_parameters.is_empty())
                    m_parameters.append(m_current_parameter);
                handle_csi(byte);
                m_state = State::Ground;
            } else if (byte < 0x20) {
                execute_control(byte);
            } else if (byte >= 0x20 && byte <= 0x2F) {
                // Intermediates: none that matter.
            } else {
                m_state = State::Ground;
            }
            break;
        case State::OSC:
            if (byte == 0x07) {
                handle_osc();
                m_state = State::Ground;
            } else if (byte == 0x1B) {
                m_state = State::OSCEscape;
            } else if (m_osc.size() < 4096) {
                m_osc.append(static_cast<char>(byte));
            }
            break;
        case State::OSCEscape:
            handle_osc();
            m_state = State::Ground;
            break;
        }
    }
}

void Terminal::execute_control(u8 byte)
{
    switch (byte) {
    case 0x07:
        if (on_bell)
            on_bell();
        break;
    case 0x08:
        if (m_cursor_column > 0)
            --m_cursor_column;
        m_wrap_pending = false;
        break;
    case 0x09: {
        int next = (m_cursor_column / 8 + 1) * 8;
        m_cursor_column = min(next, m_columns - 1);
        m_wrap_pending = false;
        break;
    }
    case 0x0A:
    case 0x0B:
    case 0x0C:
        new_line();
        break;
    case 0x0D:
        m_cursor_column = 0;
        m_wrap_pending = false;
        break;
    case 0x1B:
        m_state = State::Escape;
        break;
    default:
        break;
    }
}

void Terminal::handle_escape(u8 byte)
{
    m_state = State::Ground;
    switch (byte) {
    case '[':
        m_state = State::CSI;
        m_parameters.clear();
        m_current_parameter = -1;
        m_private_marker = false;
        return;
    case ']':
        m_state = State::OSC;
        m_osc.clear();
        return;
    case '(':
    case ')':
    case '*':
    case '+':
    case '#':
    case '%':
        m_state = State::EscapeIntermediate;
        return;
    case '7':
        m_saved_row = m_cursor_row;
        m_saved_column = m_cursor_column;
        m_saved_attributes = m_attributes;
        return;
    case '8':
        m_cursor_row = clamp(m_saved_row, 0, m_rows - 1);
        m_cursor_column = clamp(m_saved_column, 0, m_columns - 1);
        m_attributes = m_saved_attributes;
        m_wrap_pending = false;
        return;
    case 'D':
        new_line();
        return;
    case 'E':
        m_cursor_column = 0;
        new_line();
        return;
    case 'M':
        reverse_line();
        return;
    case 'c':
        reset();
        return;
    default:
        return;
    }
}

int Terminal::parameter(size_t index, int fallback) const
{
    if (index >= m_parameters.size() || m_parameters[index] <= 0)
        return fallback;
    return m_parameters[index];
}

void Terminal::set_cursor(int row, int column)
{
    int top = m_origin_mode ? m_scroll_top : 0;
    int bottom = m_origin_mode ? m_scroll_bottom : m_rows - 1;
    m_cursor_row = clamp(row + top, top, bottom);
    m_cursor_column = clamp(column, 0, m_columns - 1);
    m_wrap_pending = false;
}

void Terminal::handle_csi(u8 final)
{
    if (m_private_marker) {
        if (final == 'h' || final == 'l') {
            bool on = final == 'h';
            for (size_t i = 0; i < m_parameters.size(); ++i)
                set_private_mode(m_parameters[i], on);
        } else if (final == 'c' && on_reply) {
            // Secondary attributes: an xterm, of no great age.
            on_reply("\x1b[>0;0;0c"_sv);
        }
        return;
    }
    switch (final) {
    case 'A':
        m_cursor_row = max(m_cursor_row - parameter(0, 1), m_cursor_row >= m_scroll_top ? m_scroll_top : 0);
        m_wrap_pending = false;
        break;
    case 'B':
    case 'e':
        m_cursor_row = min(m_cursor_row + parameter(0, 1), m_cursor_row <= m_scroll_bottom ? m_scroll_bottom : m_rows - 1);
        m_wrap_pending = false;
        break;
    case 'C':
    case 'a':
        m_cursor_column = min(m_cursor_column + parameter(0, 1), m_columns - 1);
        m_wrap_pending = false;
        break;
    case 'D':
        m_cursor_column = max(m_cursor_column - parameter(0, 1), 0);
        m_wrap_pending = false;
        break;
    case 'E':
        m_cursor_row = min(m_cursor_row + parameter(0, 1), m_scroll_bottom);
        m_cursor_column = 0;
        m_wrap_pending = false;
        break;
    case 'F':
        m_cursor_row = max(m_cursor_row - parameter(0, 1), m_scroll_top);
        m_cursor_column = 0;
        m_wrap_pending = false;
        break;
    case 'G':
    case '`':
        m_cursor_column = clamp(parameter(0, 1) - 1, 0, m_columns - 1);
        m_wrap_pending = false;
        break;
    case 'd':
        m_cursor_row = clamp(parameter(0, 1) - 1, 0, m_rows - 1);
        m_wrap_pending = false;
        break;
    case 'H':
    case 'f':
        set_cursor(parameter(0, 1) - 1, parameter(1, 1) - 1);
        break;
    case 'J':
        erase_in_display(m_parameters.is_empty() ? 0 : max(m_parameters[0], 0));
        break;
    case 'K':
        erase_in_line(m_parameters.is_empty() ? 0 : max(m_parameters[0], 0));
        break;
    case 'L':
        insert_lines(parameter(0, 1));
        break;
    case 'M':
        delete_lines(parameter(0, 1));
        break;
    case 'P':
        delete_characters(parameter(0, 1));
        break;
    case '@':
        insert_characters(parameter(0, 1));
        break;
    case 'X':
        erase_characters(parameter(0, 1));
        break;
    case 'S':
        scroll_up(parameter(0, 1));
        break;
    case 'T':
        scroll_down(parameter(0, 1));
        break;
    case 'm':
        select_graphic_rendition();
        break;
    case 'r': {
        int top = parameter(0, 1) - 1;
        int bottom = parameter(1, m_rows) - 1;
        if (top < bottom && bottom < m_rows) {
            m_scroll_top = top;
            m_scroll_bottom = bottom;
            set_cursor(0, 0);
        }
        break;
    }
    case 's':
        m_saved_row = m_cursor_row;
        m_saved_column = m_cursor_column;
        break;
    case 'u':
        m_cursor_row = clamp(m_saved_row, 0, m_rows - 1);
        m_cursor_column = clamp(m_saved_column, 0, m_columns - 1);
        m_wrap_pending = false;
        break;
    case 'n':
        if (on_reply) {
            if (parameter(0, 0) == 5) {
                on_reply("\x1b[0n"_sv);
            } else if (parameter(0, 0) == 6) {
                StringBuilder builder;
                builder.appendff("\x1b[{};{}R", m_cursor_row + 1, m_cursor_column + 1);
                on_reply(builder.view());
            }
        }
        break;
    case 'c':
        if (on_reply)
            on_reply("\x1b[?1;2c"_sv);
        break;
    default:
        break;
    }
}

void Terminal::handle_osc()
{
    StringView text { m_osc.data(), m_osc.size() };
    size_t separator = 0;
    while (separator < text.length() && text[separator] != ';')
        ++separator;
    if (separator >= text.length())
        return;
    auto code = text.substring_view(0, separator);
    if (code == "0"_sv || code == "2"_sv) {
        m_title = MUST(String::from_view(text.substring_view(separator + 1, text.length() - separator - 1)));
        if (on_title_change)
            on_title_change();
    }
}

void Terminal::select_graphic_rendition()
{
    if (m_parameters.is_empty()) {
        m_attributes = {};
        return;
    }
    for (size_t i = 0; i < m_parameters.size(); ++i) {
        int code = max(m_parameters[i], 0);
        auto extended = [&](Color& color) {
            // 5;index or 2;r;g;b.
            int kind = i + 1 < m_parameters.size() ? m_parameters[i + 1] : -1;
            if (kind == 5 && i + 2 < m_parameters.size()) {
                color.kind = Color::Kind::Indexed;
                color.index = static_cast<u8>(clamp(m_parameters[i + 2], 0, 255));
                i += 2;
            } else if (kind == 2 && i + 4 < m_parameters.size()) {
                color.kind = Color::Kind::RGB;
                color.red = static_cast<u8>(clamp(m_parameters[i + 2], 0, 255));
                color.green = static_cast<u8>(clamp(m_parameters[i + 3], 0, 255));
                color.blue = static_cast<u8>(clamp(m_parameters[i + 4], 0, 255));
                i += 4;
            }
        };
        auto indexed = [](int index) {
            Color color;
            color.kind = Color::Kind::Indexed;
            color.index = static_cast<u8>(index);
            return color;
        };
        switch (code) {
        case 0:
            m_attributes = {};
            break;
        case 1:
            m_attributes.bold = true;
            break;
        case 2:
            m_attributes.faint = true;
            break;
        case 3:
            m_attributes.italic = true;
            break;
        case 4:
            m_attributes.underline = true;
            break;
        case 7:
            m_attributes.inverse = true;
            break;
        case 9:
            m_attributes.strikethrough = true;
            break;
        case 21:
        case 22:
            m_attributes.bold = false;
            m_attributes.faint = false;
            break;
        case 23:
            m_attributes.italic = false;
            break;
        case 24:
            m_attributes.underline = false;
            break;
        case 27:
            m_attributes.inverse = false;
            break;
        case 29:
            m_attributes.strikethrough = false;
            break;
        case 38:
            extended(m_attributes.foreground);
            break;
        case 39:
            m_attributes.foreground = {};
            break;
        case 48:
            extended(m_attributes.background);
            break;
        case 49:
            m_attributes.background = {};
            break;
        default:
            if (code >= 30 && code <= 37)
                m_attributes.foreground = indexed(code - 30);
            else if (code >= 40 && code <= 47)
                m_attributes.background = indexed(code - 40);
            else if (code >= 90 && code <= 97)
                m_attributes.foreground = indexed(code - 90 + 8);
            else if (code >= 100 && code <= 107)
                m_attributes.background = indexed(code - 100 + 8);
            break;
        }
    }
}

void Terminal::set_private_mode(int mode, bool on)
{
    switch (mode) {
    case 1:
        m_application_cursor_keys = on;
        break;
    case 6:
        m_origin_mode = on;
        set_cursor(0, 0);
        break;
    case 7:
        m_auto_wrap = on;
        break;
    case 25:
        m_cursor_visible = on;
        break;
    case 47:
    case 1047:
    case 1049:
        set_alternate(on);
        break;
    case 2004:
        m_bracketed_paste = on;
        break;
    default:
        break;
    }
}

void Terminal::set_alternate(bool on)
{
    if (on == m_alternate)
        return;
    if (on) {
        m_saved_row = m_cursor_row;
        m_saved_column = m_cursor_column;
        m_saved_attributes = m_attributes;
    }
    m_alternate = on;
    if (on) {
        for (auto& line : m_alternate_screen)
            line = blank_line();
        m_cursor_row = 0;
        m_cursor_column = 0;
    } else {
        m_cursor_row = clamp(m_saved_row, 0, m_rows - 1);
        m_cursor_column = clamp(m_saved_column, 0, m_columns - 1);
        m_attributes = m_saved_attributes;
    }
    m_wrap_pending = false;
}

void Terminal::put_codepoint(u32 codepoint)
{
    if (m_wrap_pending) {
        m_cursor_column = 0;
        new_line();
        m_wrap_pending = false;
    }
    auto& cell = screen()[m_cursor_row][m_cursor_column];
    cell.codepoint = codepoint;
    cell.attributes = m_attributes;
    if (m_cursor_column + 1 < m_columns)
        ++m_cursor_column;
    else if (m_auto_wrap)
        m_wrap_pending = true;
}

void Terminal::new_line()
{
    m_wrap_pending = false;
    if (m_cursor_row == m_scroll_bottom)
        scroll_up(1);
    else if (m_cursor_row + 1 < m_rows)
        ++m_cursor_row;
}

void Terminal::reverse_line()
{
    m_wrap_pending = false;
    if (m_cursor_row == m_scroll_top)
        scroll_down(1);
    else if (m_cursor_row > 0)
        --m_cursor_row;
}

void Terminal::scroll_up(int count)
{
    count = min(count, m_scroll_bottom - m_scroll_top + 1);
    auto& lines = screen();
    for (int i = 0; i < count; ++i) {
        auto gone = move(lines[static_cast<size_t>(m_scroll_top)]);
        lines.remove(static_cast<size_t>(m_scroll_top));
        if (!m_alternate && m_scroll_top == 0) {
            m_scrollback.append(move(gone));
            if (static_cast<int>(m_scrollback.size()) > m_scrollback_limit)
                m_scrollback.remove(0);
        }
        lines.insert(static_cast<size_t>(m_scroll_bottom), blank_line());
    }
}

void Terminal::scroll_down(int count)
{
    count = min(count, m_scroll_bottom - m_scroll_top + 1);
    auto& lines = screen();
    for (int i = 0; i < count; ++i) {
        lines.remove(static_cast<size_t>(m_scroll_bottom));
        lines.insert(static_cast<size_t>(m_scroll_top), blank_line());
    }
}

void Terminal::erase_in_display(int mode)
{
    auto& lines = screen();
    switch (mode) {
    case 0:
        erase_in_line(0);
        for (int row = m_cursor_row + 1; row < m_rows; ++row)
            lines[static_cast<size_t>(row)] = blank_line();
        break;
    case 1:
        erase_in_line(1);
        for (int row = 0; row < m_cursor_row; ++row)
            lines[static_cast<size_t>(row)] = blank_line();
        break;
    case 2:
    case 3:
        for (auto& line : lines)
            line = blank_line();
        if (mode == 3)
            m_scrollback.clear();
        break;
    default:
        break;
    }
}

void Terminal::erase_in_line(int mode)
{
    auto& line = screen()[m_cursor_row];
    int from = 0, to = m_columns;
    if (mode == 0)
        from = m_cursor_column;
    else if (mode == 1)
        to = m_cursor_column + 1;
    Cell blank;
    blank.attributes.background = m_attributes.background;
    for (int column = from; column < to; ++column)
        line[column] = blank;
    m_wrap_pending = false;
}

void Terminal::insert_lines(int count)
{
    if (m_cursor_row < m_scroll_top || m_cursor_row > m_scroll_bottom)
        return;
    int saved_top = m_scroll_top;
    m_scroll_top = m_cursor_row;
    scroll_down(count);
    m_scroll_top = saved_top;
    m_cursor_column = 0;
}

void Terminal::delete_lines(int count)
{
    if (m_cursor_row < m_scroll_top || m_cursor_row > m_scroll_bottom)
        return;
    int saved_top = m_scroll_top;
    m_scroll_top = m_cursor_row;
    // Not into the scrollback: this is an edit.
    auto& lines = screen();
    count = min(count, m_scroll_bottom - m_scroll_top + 1);
    for (int i = 0; i < count; ++i) {
        lines.remove(static_cast<size_t>(m_scroll_top));
        lines.insert(static_cast<size_t>(m_scroll_bottom), blank_line());
    }
    m_scroll_top = saved_top;
    m_cursor_column = 0;
}

void Terminal::insert_characters(int count)
{
    auto& line = screen()[m_cursor_row];
    count = min(count, m_columns - m_cursor_column);
    for (int column = m_columns - 1; column >= m_cursor_column + count; --column)
        line[column] = line[column - count];
    for (int column = m_cursor_column; column < m_cursor_column + count; ++column) {
        line[column] = Cell {};
        line[column].attributes.background = m_attributes.background;
    }
}

void Terminal::delete_characters(int count)
{
    auto& line = screen()[m_cursor_row];
    count = min(count, m_columns - m_cursor_column);
    for (int column = m_cursor_column; column < m_columns - count; ++column)
        line[column] = line[column + count];
    for (int column = m_columns - count; column < m_columns; ++column) {
        line[column] = Cell {};
        line[column].attributes.background = m_attributes.background;
    }
}

void Terminal::erase_characters(int count)
{
    auto& line = screen()[m_cursor_row];
    count = min(count, m_columns - m_cursor_column);
    for (int column = m_cursor_column; column < m_cursor_column + count; ++column) {
        line[column] = Cell {};
        line[column].attributes.background = m_attributes.background;
    }
    m_wrap_pending = false;
}

}
