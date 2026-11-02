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
#include <LibUI/Event.h>
#include <LibUI/Platform.h>
#include <LibUI/View.h>

namespace UI {

class Window : public Responder {
public:
    // The frame is on the screen, in points, and includes the title bar. `flags`: see
    // <LibWindow/Protocol.h>. Windows start hidden.
    Window(StringView title, Gfx::IntRect frame, u32 flags = WS::WindowTitled | WS::WindowResizable);
    ~Window() override;

    [[nodiscard]] View& root_view() { return *m_root; }
    // Under the title bar, where a window's contents go.
    [[nodiscard]] View& content_view() { return *m_content; }
    [[nodiscard]] WindowBackend& backend() { return *m_backend; }
    [[nodiscard]] u32 flags() const { return m_flags; }

    // Makes `T` fill the contents.
    template<typename T, typename... Args>
    T& set_content(Args&&... arguments)
    {
        m_content->remove_all_children();
        return m_content->add<T>(forward<Args>(arguments)...);
    }

    void show();
    void hide();
    [[nodiscard]] bool is_shown() const { return m_shown; }
    // Goes away, if nothing objects: on_close_request may say no.
    void close();
    void set_title(StringView);
    [[nodiscard]] StringView title() const { return m_title.view(); }
    [[nodiscard]] Gfx::IntRect frame() const;
    void set_frame(Gfx::IntRect);
    void center_on_screen();
    [[nodiscard]] Gfx::FloatSize size() const;
    [[nodiscard]] float title_bar_height() const;
    void set_transparent(bool);
    [[nodiscard]] bool is_transparent() const { return m_transparent; }
    // The active one, that keys go to.
    [[nodiscard]] bool is_key() const { return m_key; }

    // Keys.
    [[nodiscard]] Responder* first_responder() const { return m_first_responder; }
    bool make_first_responder(Responder*);
    // Enter presses this, Escape that, whatever has the keyboard.
    void set_default_action(Function<void()>);
    void set_cancel_action(Function<void()>);
    // The next or previous in tab order, from where the keyboard is now.
    void focus_next(bool backward);
    // Every key goes here first, while set: an open menu.
    void set_key_interceptor(Responder*);
    // Gets what falls outside every view, as it does for a window with the pointer
    // grabbed: an open menu, to know of clicks elsewhere.
    void set_pointer_catcher(View*);
    // Where sheets hang from, for a window whose toolbar reaches into its title bar.
    void set_sheet_top(float top) { m_sheet_top = top; }
    [[nodiscard]] float sheet_top() const { return m_sheet_top >= 0 ? m_sheet_top : title_bar_height(); }
    // Tabbing stays inside this, while set: a sheet.
    void set_focus_scope(View*);
    // Lays out at once, for those that must know sizes now.
    void update_layout_now();
    // Whether focus rings show now.
    [[nodiscard]] bool shows_focus() const { return m_shows_focus; }
    void set_shows_focus(bool);

    // What happens to it.
    Function<bool()> on_close_request;
    Function<void()> on_closed;
    Function<void(bool)> on_key_change;
    Function<void(Gfx::FloatSize)> on_resize;

    // Drawing.
    void add_damage(Gfx::FloatRect const&);
    void schedule_update();
    [[nodiscard]] bool update_scheduled() const { return m_update_scheduled; }
    // Lays out, draws what is damaged, and shows it, now if the last was shown.
    void update_now();
    void draw_focus_ring(Graphics&, View&);
    void set_materials(Span<MaterialRegion const>);
    // The sidebar's material down the left, this wide, however tall the window gets. (The
    // window must be transparent.)
    void set_sidebar_material(float width);

    // The press going on now will end elsewhere (a menu took the pointer, a drag or a
    // move is under way): nothing here will hear it let go.
    void forget_press();
    // The rest of this press moves the window.
    void move_by_pointer();
    // Drags, from a view that got a mouse_down: this data, with `size` of the drawing
    // following the pointer.
    void begin_drag(StringView type, ReadonlyBytes data, Gfx::FloatSize size, Function<void(Graphics&)> draw);
    Function<void(bool accepted)> on_drag_finished;

    // From the backend.
    void handle_mouse(WS::MouseKind, Gfx::FloatPoint, u32 buttons, u32 modifiers, int wheel_x, int wheel_y);
    void handle_key(bool pressed, u16 code, u32 modifiers, StringView text);
    void handle_resized(Gfx::FloatSize);
    void handle_activation(bool);
    void handle_close_request();
    void handle_presented();
    void handle_drag(WS::DragPhase, Gfx::FloatPoint, StringView type, ReadonlyBytes data);
    void handle_drag_finished(bool accepted);
    void handle_appearance_changed();

    // From views.
    void view_going_away(View&);
    void view_hidden(View&);
    [[nodiscard]] float click_distance() const;

    [[nodiscard]] Responder* next_responder() const override { return nullptr; }
    bool key_down(KeyEvent const&) override;

private:
    class RootView;

    void set_hovered(View*);
    [[nodiscard]] View* view_at(Gfx::FloatPoint);
    void update_cursor();
    void arm_tooltip();
    void hide_tooltip();
    [[nodiscard]] Vector<View*> focusable_views() const;

    String m_title;
    u32 m_flags { 0 };
    OwnPtr<WindowBackend> m_backend;
    OwnPtr<View> m_root;
    View* m_content { nullptr };
    Responder* m_first_responder { nullptr };
    Responder* m_key_interceptor { nullptr };
    View* m_focus_scope { nullptr };
    View* m_pointer_catcher { nullptr };
    float m_sheet_top { -1 };
    View* m_mouse_target { nullptr };
    View* m_hovered { nullptr };
    View* m_drop_target { nullptr };
    Function<void()> m_default_action;
    Function<void()> m_cancel_action;
    Vector<Gfx::FloatRect> m_damage;
    bool m_shown { false };
    bool m_key { false };
    bool m_transparent { false };
    bool m_shows_focus { true };
    bool m_update_scheduled { false };
    bool m_presenting { false };
    bool m_update_pending { false };
    bool m_closed { false };
    float m_sidebar_material_width { 0 };
    WS::CursorShape m_cursor { WS::CursorShape::Arrow };
    // For telling a double click.
    double m_last_press_time { -1 };
    Gfx::FloatPoint m_last_press_position;
    u32 m_last_press_button { 0 };
    int m_click_count { 0 };
    u32 m_buttons_down { 0 };
    // The drag's picture, while one goes on.
    OwnPtr<Window> m_drag_image;
    // The tooltip's.
    int m_tooltip_timer { -1 };
    OwnPtr<Window> m_tooltip;
    Gfx::FloatPoint m_pointer;
};

}
