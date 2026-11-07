/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>

namespace UI {

class Window;

// Every window this program has.
class WindowRegistry {
public:
    static void add(Window&);
    static void remove(Window&);
    [[nodiscard]] static Span<Window* const> windows();
    // The main window, as AppKit has it: the one last key, still the one menu commands are for while a panel of
    // someone else's (Help's search, say) has the keyboard.
    static void became_key(Window&);
    [[nodiscard]] static Window* main_window();
    // Redraws all of them, with tokens looked up afresh.
    static void appearance_changed();
};

}
