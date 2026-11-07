/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Geometry.h>
#include <LibUI/Window.h>

namespace UI {
class Menu;
}

namespace DesktopShell {

class AreaView;
class ToolbarView;
class ThumbnailView;

// For screenshots that are compared: the clock that names the file doesn't move.
void set_screenshots_still(bool);

// What Shift-Command-3, 4 and 5 do, as a Mac's Screenshot does: a picture of the screen, of a part of it or of a window,
// a moment of it in the corner as a thumbnail, and then a file on the desktop.
class Screenshots {
public:
    Screenshots();
    ~Screenshots();

    // The keys it answers to, to be asked for; and one of them pressed. True if it was one of them.
    void register_keys();
    bool hotkey(u16 code, u32 modifiers);

    // Shift-Command-3: all of it.
    void take_screen();
    // Shift-Command-4: a part chosen with the pointer, or (Space) a window.
    void choose_area(bool window_at_first);
    // Shift-Command-5: the toolbar.
    void show_toolbar();

    enum class Mode : u8 {
        Screen,
        Window,
        Portion,
    };
    // What the toolbar has chosen, and its Options.
    struct Options {
        Mode mode { Mode::Portion };
        bool save_to_documents { false };
        int timer_seconds { 0 };
        bool floating_thumbnail { true };
        bool remember_selection { true };
    };

    // For the views.
    void area_chosen(Gfx::IntRect points);
    void window_chosen(u32 window);
    void cancel_choosing();
    void toolbar_mode(Mode);
    void toolbar_capture();
    void toolbar_close();
    // A press on the toolbar, which is over the chooser that has the pointer: true if it was there.
    bool toolbar_press(Gfx::IntPoint screen);
    void toolbar_release(Gfx::IntPoint screen);
    void open_options(Gfx::IntPoint screen_point);
    void thumbnail_clicked();
    void thumbnail_swiped();
    [[nodiscard]] Options const& options() const { return m_options; }
    [[nodiscard]] Gfx::IntRect last_selection() const { return m_last_selection; }

private:
    void deliver(Vector<u8> png);
    void save_now();
    [[nodiscard]] String file_path() const;
    void hide_area();
    void start_after_timer(Function<void()>);

    OwnPtr<UI::Window> m_area_window;
    AreaView* m_area { nullptr };
    OwnPtr<UI::Window> m_toolbar_window;
    ToolbarView* m_toolbar { nullptr };
    OwnPtr<UI::Window> m_thumbnail_window;
    ThumbnailView* m_thumbnail { nullptr };
    OwnPtr<UI::Menu> m_options_menu;

    Options m_options;
    Gfx::IntRect m_last_selection;
    // The one taken and not yet saved: it's saved when its thumbnail goes, as on a Mac.
    Vector<u8> m_pending;
    String m_pending_name;
    int m_thumbnail_timer { -1 };
    bool m_thumbnail_bare { false };
    bool m_from_toolbar { false };
};

}
