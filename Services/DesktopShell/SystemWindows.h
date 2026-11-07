/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Desktop.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Vector.h>
#include <LibUI/Window.h>

namespace DesktopShell {

class ForceQuitList;

// The system menu's own windows: About This Computer, as About This Mac is, and Force Quit Applications.
class AboutWindow {
public:
    void show();

private:
    OwnPtr<UI::Window> m_window;
};

class ForceQuitWindow {
public:
    // Command-Option-Escape, or from the system menu.
    void show();
    // What's running, as the window server says: kept up to date while it's open.
    void set_state(Desktop::ShellState const&);

private:
    void force_quit_chosen();

    OwnPtr<UI::Window> m_window;
    ForceQuitList* m_list { nullptr };
    Vector<Desktop::ApplicationEntry> m_applications;
};

}
