/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/Motion.h>

namespace UI {

enum class ScrollerStyle : u8 {
    // Over the content, and only while scrolling.
    Overlay,
    // In a track of its own, always.
    Legacy,
};

class Scroller final : public View {
public:
    Scroller(Axis, ScrollerStyle = ScrollerStyle::Overlay);
    ~Scroller() override;

    // Where the knob's start is, of the room it can move in; how much of the track it
    // covers. Both 0 to 1.
    void set_knob(float position, float proportion);
    [[nodiscard]] float position() const { return m_position; }
    [[nodiscard]] float proportion() const { return m_proportion; }
    [[nodiscard]] ScrollerStyle style() const { return m_style; }
    // Overlay only: shown, then fading after a moment.
    void set_revealed(bool);
    void flash();

    // A new position, as the knob is dragged.
    Function<void(float)> on_scroll;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;
    void mouse_entered() override;
    void mouse_exited() override;

private:
    [[nodiscard]] Gfx::FloatRect knob_rect() const;

    Axis m_axis;
    ScrollerStyle m_style;
    float m_position { 0 };
    float m_proportion { 1 };
    float m_opacity_now { 0 };
    bool m_hovered { false };
    bool m_dragging { false };
    float m_drag_from { 0 };
    float m_drag_position { 0 };
    int m_fade_timer { -1 };
    Animation::Animator::Id m_fade_animation { 0 };
};

// Shows part of a bigger view, moved around by the wheel with momentum, and with a
// little give past the ends.
class ScrollView : public View {
public:
    ScrollView();
    ~ScrollView() override;

    // The view inside: as big as it says, at least as big as what shows.
    void set_document(NonnullOwnPtr<View>);
    template<typename T, typename... Args>
    T& make_document(Args&&... arguments)
    {
        auto* document = new T(forward<Args>(arguments)...);
        set_document(adopt_own(*static_cast<View*>(document)));
        return *document;
    }
    [[nodiscard]] View* document() const { return m_document; }
    [[nodiscard]] View& clip_view() { return *m_clip; }

    [[nodiscard]] Gfx::FloatPoint offset() const { return m_offset; }
    void scroll_to(Gfx::FloatPoint, bool animated = false);
    void scroll_rect_into_view(Gfx::FloatRect const&);
    void set_scroller_style(ScrollerStyle);
    void set_scrolls_horizontally(bool scrolls) { m_horizontal_scrolling = scrolls; }
    // Room above the content that stays still, as of a table's header.
    void set_top_inset(float);
    [[nodiscard]] float top_inset() const { return m_top_inset; }

    Function<void()> on_scroll;

    void layout() override;
    bool scroll_wheel(ScrollEvent const&) override;
    [[nodiscard]] ScrollView* as_scroll_view() override { return this; }
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::ScrollArea; }

private:
    [[nodiscard]] Gfx::FloatSize document_size() const;
    [[nodiscard]] Gfx::FloatPoint largest_offset() const;
    void apply_offset();
    void scroll_on_layer_for_a_while();
    void update_scrollers();
    void start_motion();
    void stop_motion();
    void step_motion();

    View* m_clip { nullptr };
    View* m_document { nullptr };
    Scroller* m_vertical { nullptr };
    Scroller* m_horizontal { nullptr };
    ScrollerStyle m_style { ScrollerStyle::Overlay };
    bool m_horizontal_scrolling { true };
    float m_top_inset { 0 };
    Gfx::FloatPoint m_offset;
    int m_layer_timer { -1 };
    Gfx::FloatPoint m_velocity;
    int m_motion_timer { -1 };
    double m_last_step { 0 };
    double m_last_wheel { 0 };
};

}
