/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Console/Framebuffer.h>
#include <LibBase/StringView.h>

namespace Kernel {

// A scrolling text console drawn with the built-in font: the verbose boot
// log ("-v") and the panic report.
class TextConsole {
public:
    void initialize(Framebuffer&);

    [[nodiscard]] bool is_ready() const { return m_framebuffer != nullptr; }

    void set_colors(Color foreground, Color background);
    void clear();
    void write(StringView);

private:
    void put(char);
    void new_line();
    void draw(char, u32 column, u32 row);

    Framebuffer* m_framebuffer { nullptr };
    u32 m_scale { 1 };
    u32 m_margin { 0 };
    u32 m_columns { 0 };
    u32 m_rows { 0 };
    u32 m_column { 0 };
    u32 m_row { 0 };
    Color m_foreground { 0xd8, 0xd8, 0xd8 };
    Color m_background { 0x00, 0x00, 0x00 };
};

TextConsole& boot_text_console();

}
