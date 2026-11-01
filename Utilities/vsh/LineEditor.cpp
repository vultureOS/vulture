/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LineEditor.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace Shell {

namespace {

void write_text(StringView text)
{
    size_t done = 0;
    while (done < text.length()) {
        ssize_t written = write(STDOUT_FILENO, text.characters_without_null_termination() + done, text.length() - done);
        if (written <= 0)
            return;
        done += static_cast<size_t>(written);
    }
}

// Characters a prompt shows (escape sequences take no room).
size_t visible_width(StringView text)
{
    size_t width = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '\x1b') {
            while (i < text.length() && !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')))
                ++i;
            continue;
        }
        if ((static_cast<u8>(text[i]) & 0xc0) != 0x80)
            ++width;
    }
    return width;
}

bool is_word_separator(char c)
{
    return c == ' ' || c == '\t' || c == '/' || c == '=' || c == ';' || c == '|' || c == '&';
}

}

LineEditor::LineEditor(Vector<String>& history, Completer completer, void* context)
    : m_history(history)
    , m_completer(completer)
    , m_context(context)
{
}

bool LineEditor::read_byte(char& byte)
{
    for (;;) {
        ssize_t got = read(STDIN_FILENO, &byte, 1);
        if (got == 1)
            return true;
        if (got < 0 && errno == EINTR)
            continue;
        return false;
    }
}

void LineEditor::refresh()
{
    StringBuilder output;
    output.append('\r');
    output.append(m_prompt);
    output.append(StringView(m_buffer.begin(), m_buffer.size()));
    output.append("\x1b[K"_sv);
    if (m_cursor < m_buffer.size())
        output.appendff("\x1b[{}D", m_buffer.size() - m_cursor);
    write_text(output.view());
}

void LineEditor::start_search()
{
    m_searching = true;
    m_search_failed = false;
    m_query.clear();
    m_match_index = m_history.size();
    m_line_before_search = m_buffer;
    search_refresh();
}

void LineEditor::search_refresh()
{
    refresh();
    StringBuilder output;
    output.append(m_newline);
    output.append("\x1b[K"_sv);
    output.append(m_search_failed ? "failing bck-i-search: "_sv : "bck-i-search: "_sv);
    output.append(StringView(m_query.begin(), m_query.size()));
    output.append("_\x1b[A\r"_sv);
    size_t column = m_prompt_width + m_cursor;
    if (column > 0)
        output.appendff("\x1b[{}C", column);
    write_text(output.view());
}

bool LineEditor::search_from(size_t from)
{
    StringView query(m_query.begin(), m_query.size());
    for (size_t i = min(from, m_history.size()); i-- > 0;) {
        auto found = m_history[i].view().find(query);
        if (!found.has_value())
            continue;
        m_match_index = i;
        m_buffer.clear();
        m_cursor = 0;
        insert(m_history[i].view());
        m_cursor = found.value();
        m_search_failed = false;
        return true;
    }
    m_search_failed = true;
    return false;
}

void LineEditor::end_search()
{
    m_searching = false;
    write_text(m_newline);
    write_text("\x1b[K\x1b[A"_sv);
    refresh();
}

void LineEditor::insert(StringView text)
{
    for (char c : text)
        m_buffer.insert(m_cursor++, c);
}

void LineEditor::erase_before(size_t count)
{
    while (count-- > 0 && m_cursor > 0)
        m_buffer.remove(--m_cursor);
}

void LineEditor::complete(bool listing)
{
    size_t start = m_cursor;
    while (start > 0 && m_buffer[start - 1] != ' ' && m_buffer[start - 1] != '\t' && m_buffer[start - 1] != ';' && m_buffer[start - 1] != '|' && m_buffer[start - 1] != '&')
        --start;
    // The command position: nothing but blanks (or a separator) before.
    size_t before = start;
    while (before > 0 && (m_buffer[before - 1] == ' ' || m_buffer[before - 1] == '\t'))
        --before;
    bool command_position = before == 0 || m_buffer[before - 1] == ';' || m_buffer[before - 1] == '|' || m_buffer[before - 1] == '&';
    StringView line(m_buffer.begin(), m_cursor);
    auto candidates = m_completer(m_context, line, start, command_position);
    if (candidates.is_empty()) {
        write_text("\a"_sv);
        return;
    }
    StringView typed = line.substring_view(start);
    // For files, the candidates are whole paths; the typed word is too.
    size_t common = candidates[0].length();
    for (auto const& candidate : candidates) {
        size_t same = 0;
        while (same < common && same < candidate.length() && candidate.view()[same] == candidates[0].view()[same])
            ++same;
        common = same;
    }
    if (common > typed.length()) {
        insert(candidates[0].view().substring_view(typed.length(), common - typed.length()));
        if (candidates.size() == 1 && !candidates[0].view().ends_with('/'))
            insert(" "_sv);
        refresh();
        return;
    }
    if (candidates.size() == 1) {
        if (!candidates[0].view().ends_with('/'))
            insert(" "_sv);
        refresh();
        return;
    }
    if (!listing) {
        write_text("\a"_sv);
        return;
    }
    // A second Tab lists them, in columns as wide as the terminal allows.
    struct winsize size { 25, 80, 0, 0 };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    size_t width = 0;
    for (auto const& candidate : candidates) {
        StringView name = candidate.view();
        if (auto slash = name.substring_view(0, name.length() - (name.ends_with('/') ? 1 : 0)).find_last('/'); slash.has_value())
            name = name.substring_view(slash.value() + 1);
        width = max(width, name.length());
    }
    width += 2;
    size_t columns = max<size_t>(1, size.ws_col / width);
    StringBuilder listing_text;
    listing_text.append(m_newline);
    for (size_t i = 0; i < candidates.size(); ++i) {
        StringView name = candidates[i].view();
        if (auto slash = name.substring_view(0, name.length() - (name.ends_with('/') ? 1 : 0)).find_last('/'); slash.has_value())
            name = name.substring_view(slash.value() + 1);
        listing_text.append(name);
        bool end_of_row = (i + 1) % columns == 0 || i + 1 == candidates.size();
        if (end_of_row) {
            listing_text.append(m_newline);
        } else {
            for (size_t pad = name.length(); pad < width; ++pad)
                listing_text.append(' ');
        }
    }
    write_text(listing_text.view());
    refresh();
}

Optional<String> LineEditor::read_line(StringView prompt)
{
    m_prompt = prompt;
    m_prompt_width = visible_width(prompt);
    m_buffer.clear();
    m_cursor = 0;
    m_last_was_tab = false;
    m_searching = false;

    struct termios original;
    bool terminal = tcgetattr(STDIN_FILENO, &original) == 0;
    // A new line: "\n" where the terminal adds the carriage return itself.
    m_newline = terminal && (original.c_oflag & OPOST) && (original.c_oflag & ONLCR) ? "\n"_sv : "\r\n"_sv;
    if (terminal) {
        // As zsh: no echo, no line editing or signals by the terminal; but
        // Return still arrives as a newline, so a line typed ahead for the
        // next program ends as it should.
        struct termios raw = original;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~IXON;
        raw.c_iflag |= ICRNL;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
    }
    write_text(prompt);

    size_t history_index = m_history.size();
    String saved_line;
    Optional<String> result;
    bool finished = false;
    while (!finished) {
        char c;
        if (!read_byte(c)) {
            if (m_buffer.is_empty())
                break;
            result = MUST(String::from_view(StringView(m_buffer.begin(), m_buffer.size())));
            break;
        }
        bool was_tab = m_last_was_tab;
        m_last_was_tab = false;
        if (m_searching) {
            if (c == 0x12) { // ^R again: the next older match.
                if (m_match_index > 0)
                    search_from(m_match_index);
                search_refresh();
                continue;
            }
            if (c == 0x07) { // ^G: back to the line as it was.
                m_buffer = m_line_before_search;
                m_cursor = m_buffer.size();
                end_search();
                continue;
            }
            if (c == 0x7f || c == 0x08) {
                if (!m_query.is_empty())
                    m_query.take_last();
                search_from(m_history.size());
                search_refresh();
                continue;
            }
            if (static_cast<u8>(c) >= 0x20) {
                m_query.append(c);
                // From the current match, which may still hold the longer query.
                search_from(m_match_index + 1);
                search_refresh();
                continue;
            }
            // Anything else keeps the match and does what it does.
            end_search();
        }
        switch (c) {
        case 0x12: // ^R
            start_search();
            break;
        case 0x10: // ^P: as Up.
        case 0x0e: // ^N: as Down.
            if (c == 0x10 && history_index == 0)
                break;
            if (c == 0x0e && history_index >= m_history.size())
                break;
            if (history_index == m_history.size())
                saved_line = MUST(String::from_view(StringView(m_buffer.begin(), m_buffer.size())));
            history_index += c == 0x10 ? -1 : 1;
            m_buffer.clear();
            m_cursor = 0;
            insert(history_index < m_history.size() ? m_history[history_index].view() : saved_line.view());
            refresh();
            break;
        case '\r':
        case '\n':
            write_text(m_newline);
            result = MUST(String::from_view(StringView(m_buffer.begin(), m_buffer.size())));
            finished = true;
            break;
        case 0x03: // ^C: abandon the line.
            write_text("^C"_sv);
            write_text(m_newline);
            result = String {};
            finished = true;
            break;
        case 0x04: // ^D: end of input on an empty line, else delete.
            if (m_buffer.is_empty()) {
                write_text(m_newline);
                finished = true;
                break;
            }
            if (m_cursor < m_buffer.size()) {
                m_buffer.remove(m_cursor);
                refresh();
            }
            break;
        case 0x01: // ^A
            m_cursor = 0;
            refresh();
            break;
        case 0x05: // ^E
            m_cursor = m_buffer.size();
            refresh();
            break;
        case 0x02: // ^B
            if (m_cursor > 0)
                --m_cursor;
            refresh();
            break;
        case 0x06: // ^F
            if (m_cursor < m_buffer.size())
                ++m_cursor;
            refresh();
            break;
        case 0x0b: // ^K
            while (m_buffer.size() > m_cursor)
                m_buffer.remove(m_cursor);
            refresh();
            break;
        case 0x15: // ^U
            erase_before(m_cursor);
            refresh();
            break;
        case 0x17: { // ^W
            size_t count = 0;
            size_t position = m_cursor;
            while (position > 0 && m_buffer[position - 1] == ' ') {
                --position;
                ++count;
            }
            while (position > 0 && !is_word_separator(m_buffer[position - 1])) {
                --position;
                ++count;
            }
            erase_before(count);
            refresh();
            break;
        }
        case 0x0c: // ^L
            write_text("\x1b[H\x1b[2J"_sv);
            refresh();
            break;
        case 0x7f:
        case 0x08:
            erase_before(1);
            refresh();
            break;
        case '\t':
            complete(was_tab);
            m_last_was_tab = true;
            break;
        case 0x1b: {
            char first, second;
            if (!read_byte(first) || !read_byte(second))
                break;
            if (first != '[' && first != 'O')
                break;
            char key = second;
            if (second >= '0' && second <= '9') {
                char tilde;
                if (!read_byte(tilde))
                    break;
                key = second == '3' ? 'X' : (second == '1' || second == '7' ? 'H' : (second == '4' || second == '8' ? 'F' : '\0'));
            }
            switch (key) {
            case 'A': // Up: the previous history entry.
            case 'B': // Down
                if (key == 'A' && history_index == 0)
                    break;
                if (key == 'B' && history_index >= m_history.size())
                    break;
                if (history_index == m_history.size())
                    saved_line = MUST(String::from_view(StringView(m_buffer.begin(), m_buffer.size())));
                history_index += key == 'A' ? -1 : 1;
                m_buffer.clear();
                m_cursor = 0;
                insert(history_index < m_history.size() ? m_history[history_index].view() : saved_line.view());
                refresh();
                break;
            case 'C':
                if (m_cursor < m_buffer.size())
                    ++m_cursor;
                refresh();
                break;
            case 'D':
                if (m_cursor > 0)
                    --m_cursor;
                refresh();
                break;
            case 'H':
                m_cursor = 0;
                refresh();
                break;
            case 'F':
                m_cursor = m_buffer.size();
                refresh();
                break;
            case 'X':
                if (m_cursor < m_buffer.size()) {
                    m_buffer.remove(m_cursor);
                    refresh();
                }
                break;
            default:
                break;
            }
            break;
        }
        default:
            if (static_cast<u8>(c) >= 0x20) {
                insert(StringView(&c, 1));
                if (m_cursor == m_buffer.size())
                    write_text(StringView(&c, 1));
                else
                    refresh();
            }
        }
    }
    if (terminal)
        tcsetattr(STDIN_FILENO, TCSADRAIN, &original);
    return result;
}

}
