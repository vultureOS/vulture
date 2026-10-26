/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Console.h>

namespace Boot {

Console* g_console = nullptr;

void Console::write(StringView text)
{
    size_t index = 0;
    while (index < text.length()) {
        u8 lead = static_cast<u8>(text[index]);
        size_t remaining = text.length() - index;
        u32 code_point = 0xfffd;
        size_t sequence_length = 1;

        if (lead < 0x80) {
            code_point = lead;
        } else if ((lead & 0xe0) == 0xc0 && remaining >= 2) {
            code_point = ((lead & 0x1fu) << 6) | (static_cast<u8>(text[index + 1]) & 0x3fu);
            sequence_length = 2;
        } else if ((lead & 0xf0) == 0xe0 && remaining >= 3) {
            code_point = ((lead & 0x0fu) << 12) | ((static_cast<u8>(text[index + 1]) & 0x3fu) << 6) | (static_cast<u8>(text[index + 2]) & 0x3fu);
            sequence_length = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            // Outside the Basic Multilingual Plane: UCS-2 cannot show it.
            sequence_length = min<size_t>(4, remaining);
        }

        if (code_point == '\n')
            append(u'\r');
        append(static_cast<char16_t>(code_point));
        index += sequence_length;
    }
    flush();
}

void Console::write_ucs2(char16_t const* text)
{
    flush();
    m_output->OutputString(m_output, text);
}

void Console::set_color(UEFI::TextColor foreground, UEFI::TextColor background)
{
    flush();
    m_output->SetAttribute(m_output, to_underlying(foreground) | (to_underlying(background) & 0x7u) << 4);
}

void Console::clear()
{
    m_used = 0;
    m_output->ClearScreen(m_output);
}

void Console::append(char16_t character)
{
    // Keep one slot for the terminating null.
    if (m_used == array_size(m_buffer) - 1)
        flush();
    m_buffer[m_used++] = character;
}

void Console::flush()
{
    if (m_used == 0)
        return;
    m_buffer[m_used] = u'\0';
    m_output->OutputString(m_output, m_buffer);
    m_used = 0;
}

}
