/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Optional.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Shell {

// Reads a line from the terminal with editing as in zsh or bash (emacs
// keys): arrows, Home/End, ^A ^E ^B ^F ^K ^U ^W ^L ^D, Backspace and
// Delete, history with up and down, and Tab completion.
class LineEditor {
public:
    // The candidates for completing the word before the cursor (whose
    // first character is at `word_start`).
    using Completer = Vector<String> (*)(void* context, StringView line, size_t word_start, bool command_position);

    LineEditor(Vector<String>& history, Completer, void* context);

    // Nothing at end of input (^D on an empty line). ^C gives an empty line.
    Optional<String> read_line(StringView prompt);

private:
    void refresh();
    void insert(StringView);
    void complete(bool listing);
    void erase_before(size_t count);
    bool read_byte(char&);

    Vector<String>& m_history;
    Completer m_completer;
    void* m_context;
    StringView m_prompt;
    size_t m_prompt_width { 0 };
    Vector<char> m_buffer;
    size_t m_cursor { 0 };
    bool m_last_was_tab { false };
};

}
