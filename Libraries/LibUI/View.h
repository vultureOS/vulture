/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAnimation/Curve.h>
#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Geometry.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Event.h>
#include <LibUI/Graphics.h>
#include <LibWindow/Protocol.h>

// The tree of what is in a window. Coordinates run from the top-left corner, in
// points. A view's frame is its alignment rectangle: layouts line up the visible
// edges of controls, and what falls outside them (a shadow, a focus ring) is the
// view's overflow.

namespace UI {

class Layout;
class RadioButton;
class ScrollView;
struct ServerLayer;

class View;
class Window;

struct Insets {
    float top { 0 };
    float left { 0 };
    float bottom { 0 };
    float right { 0 };

    [[nodiscard]] static constexpr Insets all(float value) { return { value, value, value, value }; }
};

// Whoever may handle a key or a command: passes it on otherwise, along the chain
// (a view, its parent, and so on up to the window).
class Responder {
public:
    virtual ~Responder() = default;

    [[nodiscard]] virtual Responder* next_responder() const { return nullptr; }
    // (There is no RTTI to ask with.)
    [[nodiscard]] virtual View* as_view() { return nullptr; }
    [[nodiscard]] virtual bool accepts_first_responder() const { return false; }
    virtual void did_become_first_responder() { }
    virtual void did_resign_first_responder() { }
    // True if handled. Otherwise it goes on to the next.
    virtual bool key_down(KeyEvent const&) { return false; }
    virtual bool key_up(KeyEvent const&) { return false; }
    // "copy", "paste", "undo", "selectAll", ...: from a menu or a shortcut.
    virtual bool perform_command(StringView) { return false; }
    [[nodiscard]] virtual bool can_perform_command(StringView) const { return false; }
};

class View : public Responder {
public:
    View();
    ~View() override;

    View(View const&) = delete;
    View& operator=(View const&) = delete;

    // The tree.
    [[nodiscard]] View* parent() const { return m_parent; }
    [[nodiscard]] Window* window() const;
    [[nodiscard]] Vector<NonnullOwnPtr<View>> const& children() const { return m_children; }

    template<typename T, typename... Args>
    T& add(Args&&... arguments)
    {
        auto* child = new T(forward<Args>(arguments)...);
        adopt_child(adopt_own(*static_cast<View*>(child)));
        return *child;
    }
    void adopt_child(NonnullOwnPtr<View>);
    void adopt_child_at(size_t index, NonnullOwnPtr<View>);
    [[nodiscard]] NonnullOwnPtr<View> take_child(View&);
    void remove_child(View&);
    void remove_all_children();
    void bring_child_to_front(View&);

    // Where it is.
    [[nodiscard]] Gfx::FloatRect frame() const { return m_frame; }
    void set_frame(Gfx::FloatRect const&);
    void set_position(Gfx::FloatPoint point) { set_frame({ point.x, point.y, m_frame.width, m_frame.height }); }
    void set_size(Gfx::FloatSize size) { set_frame({ m_frame.x, m_frame.y, size.width, size.height }); }
    [[nodiscard]] float width() const { return m_frame.width; }
    [[nodiscard]] float height() const { return m_frame.height; }
    // Its own coordinates: (0, 0) at the top-left, unless it scrolls.
    [[nodiscard]] Gfx::FloatRect bounds() const { return { m_bounds_origin.x, m_bounds_origin.y, m_frame.width, m_frame.height }; }
    [[nodiscard]] Gfx::FloatPoint bounds_origin() const { return m_bounds_origin; }
    void set_bounds_origin(Gfx::FloatPoint);
    [[nodiscard]] Gfx::FloatPoint to_window(Gfx::FloatPoint) const;
    [[nodiscard]] Gfx::FloatPoint from_window(Gfx::FloatPoint) const;
    [[nodiscard]] Gfx::FloatRect rect_in_window() const;

    // How it looks.
    [[nodiscard]] bool is_hidden() const { return m_hidden; }
    void set_hidden(bool);
    [[nodiscard]] bool is_visible() const;
    [[nodiscard]] float opacity() const { return m_opacity; }
    void set_opacity(float);
    // An offset that layout knows nothing about: for animations.
    [[nodiscard]] Gfx::FloatPoint translation() const { return m_translation; }
    void set_translation(Gfx::FloatPoint);
    [[nodiscard]] bool clips_children() const { return m_clips_children; }
    void set_clips_children(bool clips) { m_clips_children = clips; }
    // Its subtree is kept as a picture, and the picture moved and faded: what stays
    // cheap while animating.
    void set_layer_backed(bool);
    [[nodiscard]] bool is_layer_backed() const { return m_layer_backed; }
    // Its subtree goes in a surface of its own in the window server, which moves and fades it by itself, without this program
    // having to draw it again or even be running. It looks the same; and where there's no server, it's just drawn with the rest.
    void set_wants_layer(bool);
    [[nodiscard]] bool wants_layer() const { return m_wants_layer; }
    [[nodiscard]] bool has_server_layer() const { return m_server_layer.ptr() != nullptr; }
    // And what's in it scrolls there: moving the bounds origin draws nothing, and more than can be seen is kept ready.
    void set_scrolls_on_layer(bool);
    // To be done by the window server if it can be, so that it carries on while this program's busy; or here if not. In the end
    // it's where it was told to go, and `finished` is told, here, when this program can hear of it.
    void animate_translation(Gfx::FloatPoint from, Gfx::FloatPoint to, Animation::Timing const&, Function<void()> finished = nullptr);
    void animate_opacity(float from, float to, Animation::Timing const&, Function<void()> finished = nullptr);
    // Round and round about its middle, in so many steps, for as long as it has a layer.
    void rotate_forever(float seconds, int steps);
    // It has a layer now, new: for what has to be asked of one to start with.
    virtual void layer_was_made() { }
    void set_needs_display();
    void set_needs_display(Gfx::FloatRect const&);
    virtual void draw(Graphics&) { }
    virtual void draw_over_children(Graphics&) { }
    // How far it may draw outside its frame.
    [[nodiscard]] virtual Insets overflow() const { return {}; }
    // The tokens may have changed: for those that remember anything from them.
    virtual void appearance_changed() { }

    // Layout.
    void set_layout(OwnPtr<Layout>);
    template<typename T, typename... Args>
    T& set_layout(Args&&... arguments)
    {
        auto* layout = new T(forward<Args>(arguments)...);
        set_layout(adopt_own(*static_cast<Layout*>(layout)));
        return *layout;
    }
    [[nodiscard]] Layout* layout_manager() const { return m_layout.ptr(); }
    void set_needs_layout();
    void layout_if_needed();
    // Places the children. By default: by its layout, if it has one.
    virtual void layout();
    // How big it would like to be; negative in a direction where it has no opinion.
    [[nodiscard]] virtual Gfx::FloatSize intrinsic_size() const { return { -1, -1 }; }
    // With none, in either direction, neither it nor what's in it is drawn, so nobody can find or press them either.
    [[nodiscard]] bool has_room_to_be_seen() const;
    // What layouts go by: the fixed sizes if set, or else intrinsic_size, or else what
    // its layout needs.
    [[nodiscard]] Gfx::FloatSize preferred_size() const;
    void set_fixed_width(float width);
    void set_fixed_height(float height);
    void set_fixed_size(Gfx::FloatSize size)
    {
        set_fixed_width(size.width);
        set_fixed_height(size.height);
    }
    void set_minimum_size(Gfx::FloatSize size) { m_minimum = size; }
    [[nodiscard]] Gfx::FloatSize minimum_size() const { return m_minimum; }
    // Its share of any room left over, along a stack's axis.
    void set_stretch(float horizontal, float vertical)
    {
        m_stretch_x = horizontal;
        m_stretch_y = vertical;
    }
    [[nodiscard]] float horizontal_stretch() const { return m_stretch_x; }
    [[nodiscard]] float vertical_stretch() const { return m_stretch_y; }
    // Where text in it sits from its top, for lining up rows; negative if it has none.
    [[nodiscard]] virtual float baseline() const { return -1; }

    // Events. A view that returns false from mouse_down lets its parent have it.
    [[nodiscard]] virtual View* hit_test(Gfx::FloatPoint in_parent);
    virtual bool mouse_down(MouseEvent const&) { return false; }
    virtual void mouse_up(MouseEvent const&) { }
    virtual void mouse_moved(MouseEvent const&) { }
    virtual void mouse_dragged(MouseEvent const&) { }
    virtual void mouse_entered() { }
    virtual void mouse_exited() { }
    virtual bool scroll_wheel(ScrollEvent const&) { return false; }
    [[nodiscard]] virtual WS::CursorShape cursor() const { return WS::CursorShape::Arrow; }
    [[nodiscard]] Responder* next_responder() const override;
    [[nodiscard]] View* as_view() override { return this; }
    [[nodiscard]] bool is_first_responder() const;
    bool become_first_responder();

    // There is no RTTI to ask with.
    [[nodiscard]] virtual RadioButton* as_radio_button() { return nullptr; }
    [[nodiscard]] virtual ScrollView* as_scroll_view() { return nullptr; }

    // Tab goes through those that can_take_key_focus, in order of the tree.
    [[nodiscard]] virtual bool can_take_key_focus() const { return accepts_first_responder() && is_enabled() && is_visible(); }
    [[nodiscard]] virtual bool shows_focus_ring() const { return true; }
    // The ring's shape, in the view's coordinates.
    [[nodiscard]] virtual Gfx::FloatRect focus_ring_rect() const { return { 0, 0, width(), height() }; }
    [[nodiscard]] virtual float focus_ring_radius() const { return 0; }

    // Enabled, it and all above it.
    void set_enabled(bool);
    [[nodiscard]] bool is_enabled() const;
    [[nodiscard]] bool is_self_enabled() const { return m_enabled; }

    // Drag and drop, as a destination: return true while what is over it would be
    // accepted (of this type); it gets the data if released here.
    [[nodiscard]] virtual bool accepts_drop(StringView type) const
    {
        (void)type;
        return false;
    }
    virtual void drag_entered() { }
    virtual void drag_exited() { }
    virtual void perform_drop(StringView type, ReadonlyBytes data)
    {
        (void)type;
        (void)data;
    }

    // Accessibility: what it is, for whoever cannot see it.
    [[nodiscard]] virtual Accessibility::Role accessibility_role() const { return Accessibility::Role::Group; }
    [[nodiscard]] virtual String accessibility_label() const;
    [[nodiscard]] virtual String accessibility_value() const;
    void set_accessibility_label(StringView);
    // Presses it, as far as a screen reader is concerned.
    virtual bool accessibility_press() { return false; }
    // What it has inside that aren't views of their own: a table's rows, a menu's items.
    virtual void accessibility_parts(Vector<Accessibility::Node>&) { }

    void set_tooltip(StringView);
    [[nodiscard]] StringView tooltip() const { return m_tooltip.view(); }

    // What lays out, draws and dispatches to it: not for general use.
    void draw_tree(Graphics&, Gfx::FloatRect const& dirty);
    void propagate_appearance_change();
    void did_join_window(Window*);
    [[nodiscard]] bool needs_layout() const { return m_needs_layout; }
    void cancel_layer();

private:
    friend class Window;

    void draw_contents(Graphics&, Gfx::FloatRect const& dirty, bool scrolls_on_layer = false);
    void invalidate_ancestor_layers();
    void damage_without_redrawing_layer();

    View* m_parent { nullptr };
    Window* m_window { nullptr };
    Vector<NonnullOwnPtr<View>> m_children;
    OwnPtr<Layout> m_layout;
    Gfx::FloatRect m_frame;
    Gfx::FloatPoint m_bounds_origin;
    Gfx::FloatPoint m_translation;
    Gfx::FloatSize m_fixed { -1, -1 };
    Gfx::FloatSize m_minimum { 0, 0 };
    float m_opacity { 1 };
    float m_stretch_x { 0 };
    float m_stretch_y { 0 };
    bool m_hidden { false };
    bool m_enabled { true };
    bool m_clips_children { false };
    bool m_needs_layout { true };
    bool m_layer_backed { false };
    bool m_layer_dirty { true };
    bool m_wants_layer { false };
    bool m_scrolls_on_layer { false };
    View* m_planned_layer_parent { nullptr };
    OwnPtr<ServerLayer> m_server_layer;
    RefPtr<Gfx::Bitmap> m_layer;
    String m_accessibility_label;
    String m_tooltip;
};

}
