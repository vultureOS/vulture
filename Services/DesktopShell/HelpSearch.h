/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibJSON/JSON.h>
#include <LibUI/Window.h>

namespace DesktopShell {

class HelpView;

// Every application's Help menu, as on a Mac: a search field, and the application's own menu commands that match what's
// typed, any of them chosen from there.
class HelpSearch {
public:
    struct Command {
        String title;
        // The menus it's in, as "Edit" or "Format ▸ Font".
        String path;
        u32 id { 0 };
        bool enabled { true };
    };

    // The active application's menus, as published.
    void set_menus(JSON::Value const&);
    void open(Gfx::IntPoint at, Function<void(u32)> choose, Function<void()> closed);
    void close();
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] Vector<Command> const& commands() const { return m_commands; }

private:
    void collect(JSON::Value const& menu, StringView path);

    Vector<Command> m_commands;
    OwnPtr<UI::Window> m_window;
    HelpView* m_view { nullptr };
    Function<void(u32)> m_choose;
    Function<void()> m_closed;
};

}
