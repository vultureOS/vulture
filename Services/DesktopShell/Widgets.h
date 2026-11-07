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
#include <LibGfx/Geometry.h>
#include <LibUI/Window.h>

namespace UI {
class Graphics;
class Menu;
}

namespace DesktopShell {

class DesktopWidgetView;
class GalleryView;

enum class WidgetSize : u8 {
    Small,
    Medium,
    Large,
};

[[nodiscard]] Gfx::FloatSize widget_size(WidgetSize);
[[nodiscard]] StringView widget_size_name(WidgetSize);

// For screenshots that are compared: the clock on a widget doesn't move, nor what it measures.
void set_widgets_still(bool);

// A kind of widget, as WidgetKit's are: what an application shows of itself, at each size it comes in. Drawn in full
// colour, or as the desktop's are while windows are in front: in one tint, faded back.
class WidgetKind {
public:
    virtual ~WidgetKind() = default;
    [[nodiscard]] virtual StringView identifier() const = 0;
    [[nodiscard]] virtual StringView application() const = 0;
    [[nodiscard]] virtual StringView name() const = 0;
    [[nodiscard]] virtual StringView description() const = 0;
    [[nodiscard]] virtual bool comes_in(WidgetSize) const = 0;
    virtual void draw(UI::Graphics&, Gfx::FloatRect, WidgetSize, bool full_color) const = 0;
    // What it says, for the accessibility tree.
    [[nodiscard]] virtual String summary() const = 0;
    // How often what it shows changes, in seconds.
    [[nodiscard]] virtual int refresh_seconds() const { return 60; }
};

struct PlacedWidget {
    String kind;
    WidgetSize size { WidgetSize::Small };
    // On the desktop: where its top left is, on the screen.
    Gfx::IntPoint at;
};

// The widgets on the desktop and in Notification Center, and the gallery they're chosen from (Edit Widgets), as
// Sonoma's are. Kept in com.vulture.widgets, with the Desktop & Dock pane's settings for them.
class Widgets {
public:
    Widgets();
    ~Widgets();

    [[nodiscard]] Vector<PlacedWidget> const& in_center() const { return m_center; }
    [[nodiscard]] WidgetKind const* kind(StringView identifier) const;
    void draw(UI::Graphics&, Gfx::FloatRect, PlacedWidget const&, bool full_color) const;
    // Notification Center's: one taken away, or one's menu (sizes, Remove Widget, Edit Widgets...).
    void remove_from_center(size_t index);
    void open_center_menu(UI::Window& owner, Gfx::IntPoint screen_point, size_t index);

    // Edit Widgets: the gallery, at the bottom of the screen. What's chosen there goes on the desktop or, from
    // Notification Center, there.
    void edit(bool for_center);
    void finish_editing();
    [[nodiscard]] bool is_editing() const { return m_editing; }
    [[nodiscard]] bool is_editing_center() const { return m_editing && m_for_center; }
    void add(StringView kind, WidgetSize);

    // The desktop is what's in front (it was clicked, or nothing else has a window): widgets on it in full colour.
    void set_desktop_active(bool);

    Function<void()> on_center_changed;
    // The gallery has gone: chosen from Notification Center, or not.
    Function<void(bool for_center)> on_editing_done;
    // A second has gone by: what Notification Center shows of them may have changed.
    Function<void()> on_center_tick;

private:
    friend class DesktopWidgetView;
    friend class GalleryView;

    void load();
    void save();
    void show_desktop();
    void remove_from_desktop(size_t index);
    void resize(bool center, size_t index, WidgetSize);
    void moved(size_t index, Gfx::IntPoint);
    void open_desktop_menu(size_t index, Gfx::IntPoint screen_point);
    [[nodiscard]] bool full_color_on_desktop() const;
    [[nodiscard]] Gfx::IntPoint free_place(WidgetSize) const;
    void tick();

    Vector<NonnullOwnPtr<WidgetKind>> m_kinds;
    Vector<PlacedWidget> m_desktop;
    Vector<PlacedWidget> m_center;
    Vector<OwnPtr<UI::Window>> m_desktop_windows;
    Vector<DesktopWidgetView*> m_desktop_views;
    OwnPtr<UI::Window> m_gallery_window;
    GalleryView* m_gallery { nullptr };
    OwnPtr<UI::Menu> m_menu;
    bool m_editing { false };
    bool m_for_center { false };
    bool m_desktop_active { false };
    bool m_on_desktop { true };
    // "automatic", "monochrome" or "fullColor".
    String m_style;
    int m_timer { -1 };
};

}
