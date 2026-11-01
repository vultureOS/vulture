/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Menu.h>
#include <LibUI/TableView.h>
#include <LibUI/TextField.h>
#include <LibUI/Window.h>

// What comes over a window: sheets, alerts, popovers; and the standard panels for
// choosing a file, a color or a font.

namespace UI {

// Slides down over a window, and everything under it waits. One per window.
class Sheet {
public:
    // Takes over until dismiss(). The content is `size`, in points.
    static void present(Window&, NonnullOwnPtr<View> content, Gfx::FloatSize size, Function<void()> on_dismissed = nullptr);
    static void dismiss(Window&);
    [[nodiscard]] static bool is_presented(Window&);
};

// A question, with buttons to answer it: macOS's centered form, over the window.
class Alert {
public:
    Alert(StringView message, StringView information = {});

    // The first is the default, unless it says otherwise. Answers go by index.
    void add_button(StringView title, bool is_default = false);
    void set_symbol(Symbol);
    // Comes and goes by itself; `then` gets the index of the button pressed.
    void show(Window&, Function<void(size_t)> then);

private:
    String m_message;
    String m_information;
    struct ButtonDefinition {
        String title;
        bool is_default;
    };
    Vector<ButtonDefinition> m_buttons;
    Symbol m_symbol { Symbol::Question };
};

enum class ArrowEdge : u8 {
    Top,
    Bottom,
};

// A small window of its own beside what it points at, gone on a click anywhere else.
class Popover {
public:
    Popover();
    ~Popover();

    // `content` of this size; `anchor` is in `owner`'s points.
    void show(Window& owner, NonnullOwnPtr<View> content, Gfx::FloatSize size, Gfx::FloatRect anchor, ArrowEdge = ArrowEdge::Top);
    void close();
    [[nodiscard]] bool is_shown() const { return m_window.ptr() != nullptr; }
    [[nodiscard]] Window* window() const { return m_window.ptr(); }

    Function<void()> on_close;

private:
    class Frame;
    class KeyForwarder;

    OwnPtr<Window> m_window;
    OwnPtr<KeyForwarder> m_forwarder;
    Window* m_owner { nullptr };
};

// Choosing a file, in a sheet.
class FilePanel {
public:
    enum class Kind : u8 {
        Open,
        Save,
    };
    // `then` gets the whole path chosen, or empty if cancelled.
    static void run(Window&, Kind, StringView directory, StringView suggested_name, Function<void(StringView)> then);
};

// The system color panel: there is one, and it works on one color well at a time.
class ColorPanel {
public:
    [[nodiscard]] static ColorPanel& the();

    void show(Gfx::Color initial, Function<void(Gfx::Color)> on_change);
    void close();
    [[nodiscard]] Window* window() const { return m_window.ptr(); }

private:
    ColorPanel() = default;

    OwnPtr<Window> m_window;
};

// Likewise for fonts.
class FontPanel {
public:
    [[nodiscard]] static FontPanel& the();

    void show(FontSpec initial, Function<void(FontSpec)> on_change);
    void close();
    [[nodiscard]] Window* window() const { return m_window.ptr(); }

private:
    FontPanel() = default;

    OwnPtr<Window> m_window;
};

}
