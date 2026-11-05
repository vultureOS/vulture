/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Platform.h>
#include <LibUI/Window.h>
#include <LibBase/Math.h>
#include <LibUI/Platform.h>
#include <LibUI/ScrollView.h>

namespace UI {

using namespace Gfx;
namespace Math = Base::Math;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

}

Scroller::Scroller(Axis axis, ScrollerStyle style)
    : m_axis(axis)
    , m_style(style)
{
    m_opacity_now = style == ScrollerStyle::Legacy ? 1 : 0;
}

Scroller::~Scroller()
{
    if (m_fade_timer >= 0)
        Platform::the().cancel_timer(m_fade_timer);
    if (m_fade_animation)
        motion().cancel(m_fade_animation);
}

FloatSize Scroller::intrinsic_size() const
{
    float across = tokens().metric(m_style == ScrollerStyle::Overlay ? "scroller.overlay"_sv : "scroller.legacy"_sv);
    return m_axis == Axis::Vertical ? FloatSize { across, -1 } : FloatSize { -1, across };
}

void Scroller::set_knob(float position, float proportion)
{
    m_position = clamp(position, 0.0f, 1.0f);
    m_proportion = clamp(proportion, 0.0f, 1.0f);
    set_needs_display();
}

void Scroller::set_revealed(bool revealed)
{
    if (m_fade_timer >= 0) {
        Platform::the().cancel_timer(m_fade_timer);
        m_fade_timer = -1;
    }
    if (m_fade_animation) {
        motion().cancel(m_fade_animation);
        m_fade_animation = 0;
    }
    m_opacity_now = revealed ? 1 : 0;
    set_needs_display();
}

void Scroller::flash()
{
    if (m_style != ScrollerStyle::Overlay)
        return;
    set_revealed(true);
    if (held_still())
        return;
    auto delay = static_cast<u64>(tokens().metric("scroller.fadeDelayMilliseconds"_sv));
    m_fade_timer = Platform::the().add_timer(delay, false, [this] {
        m_fade_timer = -1;
        if (m_hovered || m_dragging)
            return;
        m_fade_animation = animate(Animation::Timing::ease_out(tokens().metric("scroller.fadeSeconds"_sv)), [this](float progress) {
            m_opacity_now = 1 - progress;
            set_needs_display();
        }, [this] { m_fade_animation = 0; });
    });
}

FloatRect Scroller::knob_rect() const
{
    auto const& t = tokens();
    float length = m_axis == Axis::Vertical ? height() : width();
    float across = m_axis == Axis::Vertical ? width() : height();
    float thickness;
    if (m_style == ScrollerStyle::Legacy)
        thickness = t.metric("scroller.knobLegacy"_sv);
    else
        thickness = t.metric((m_hovered || m_dragging) ? "scroller.knobHover"_sv : "scroller.knob"_sv);
    float knob_length = max(length * m_proportion, t.metric("scroller.minimumKnob"_sv));
    float start = (length - knob_length) * m_position;
    float cross = (across - thickness) / 2;
    if (m_axis == Axis::Vertical)
        return { cross, start, thickness, knob_length };
    return { start, cross, knob_length, thickness };
}

void Scroller::draw(Graphics& graphics)
{
    auto const& t = tokens();
    if (m_proportion >= 1 && m_style == ScrollerStyle::Overlay)
        return;
    if (m_style == ScrollerStyle::Legacy) {
        FloatRect track { 0, 0, width(), height() };
        if (m_axis == Axis::Vertical)
            track = { 1, 0, width() - 1, height() };
        graphics.fill(track, t.color("scroller.legacyTrack"_sv));
        if (m_axis == Axis::Vertical)
            graphics.fill({ 0, 0, graphics.pixel(), height() }, t.color("scroller.legacyBorder"_sv));
        else
            graphics.fill({ 0, 0, width(), graphics.pixel() }, t.color("scroller.legacyBorder"_sv));
        auto knob = knob_rect();
        graphics.fill_rounded(knob, min(knob.width, knob.height) / 2, t.color("scroller.legacyKnob"_sv));
        return;
    }
    if (m_opacity_now <= 0)
        return;
    auto knob = knob_rect();
    graphics.fill_rounded(knob, min(knob.width, knob.height) / 2, t.color("scroller.overlayKnob"_sv).with_opacity(m_opacity_now));
}

bool Scroller::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft || m_proportion >= 1)
        return false;
    set_revealed(true);
    auto knob = knob_rect();
    float at = m_axis == Axis::Vertical ? event.position.y : event.position.x;
    float knob_start = m_axis == Axis::Vertical ? knob.y : knob.x;
    float knob_length = m_axis == Axis::Vertical ? knob.height : knob.width;
    float length = m_axis == Axis::Vertical ? height() : width();
    if (at < knob_start || at >= knob_start + knob_length) {
        // In the track: a jump there, then drags from there.
        float wanted = (at - knob_length / 2) / max(length - knob_length, 1.0f);
        m_position = clamp(wanted, 0.0f, 1.0f);
        if (on_scroll)
            on_scroll(m_position);
    }
    m_dragging = true;
    m_drag_from = at;
    m_drag_position = m_position;
    set_needs_display();
    return true;
}

void Scroller::mouse_dragged(MouseEvent const& event)
{
    if (!m_dragging)
        return;
    auto knob = knob_rect();
    float length = m_axis == Axis::Vertical ? height() : width();
    float knob_length = m_axis == Axis::Vertical ? knob.height : knob.width;
    float at = m_axis == Axis::Vertical ? event.position.y : event.position.x;
    float position = clamp(m_drag_position + (at - m_drag_from) / max(length - knob_length, 1.0f), 0.0f, 1.0f);
    if (on_scroll)
        on_scroll(position);
}

void Scroller::mouse_up(MouseEvent const&)
{
    m_dragging = false;
    flash();
    set_needs_display();
}

void Scroller::mouse_entered()
{
    m_hovered = true;
    if (m_style == ScrollerStyle::Overlay && m_proportion < 1)
        set_revealed(true);
}

void Scroller::mouse_exited()
{
    m_hovered = false;
    flash();
    set_needs_display();
}

// ---- Scroll view

class ClipView final : public View {
public:
    ClipView() { set_clips_children(true); }
};

ScrollView::ScrollView()
{
    set_clips_children(true);
    m_clip = &add<ClipView>();
    m_vertical = &add<Scroller>(Axis::Vertical, m_style);
    m_horizontal = &add<Scroller>(Axis::Horizontal, m_style);
    m_vertical->on_scroll = [this](float position) {
        auto largest = largest_offset();
        scroll_to({ m_offset.x, position * largest.y });
    };
    m_horizontal->on_scroll = [this](float position) {
        auto largest = largest_offset();
        scroll_to({ position * largest.x, m_offset.y });
    };
}

ScrollView::~ScrollView()
{
    stop_motion();
    if (m_layer_timer >= 0)
        Platform::the().cancel_timer(m_layer_timer);
}

void ScrollView::set_document(NonnullOwnPtr<View> document)
{
    if (m_document)
        m_clip->remove_child(*m_document);
    m_document = document.ptr();
    m_clip->adopt_child(move(document));
    set_needs_layout();
}

void ScrollView::set_scroller_style(ScrollerStyle style)
{
    if (style == m_style)
        return;
    m_style = style;
    remove_child(*m_vertical);
    remove_child(*m_horizontal);
    m_vertical = &add<Scroller>(Axis::Vertical, m_style);
    m_horizontal = &add<Scroller>(Axis::Horizontal, m_style);
    m_vertical->on_scroll = [this](float position) { scroll_to({ m_offset.x, position * largest_offset().y }); };
    m_horizontal->on_scroll = [this](float position) { scroll_to({ position * largest_offset().x, m_offset.y }); };
    set_needs_layout();
}

void ScrollView::set_top_inset(float inset)
{
    m_top_inset = inset;
    set_needs_layout();
}

FloatSize ScrollView::document_size() const
{
    if (!m_document)
        return {};
    auto wanted = m_document->preferred_size();
    auto clip = m_clip->frame().size();
    return { max(wanted.width, clip.width), max(wanted.height, clip.height) };
}

FloatPoint ScrollView::largest_offset() const
{
    auto wanted = document_size();
    auto clip = m_clip->frame().size();
    return { max(wanted.width - clip.width, 0.0f), max(wanted.height - clip.height, 0.0f) };
}

void ScrollView::layout()
{
    auto const& t = tokens();
    float across = t.metric(m_style == ScrollerStyle::Overlay ? "scroller.overlay"_sv : "scroller.legacy"_sv);
    FloatRect clip { 0, m_top_inset, width(), height() - m_top_inset };
    bool legacy = m_style == ScrollerStyle::Legacy;

    // A document's size may depend on how wide it is shown: a first pass at this width.
    if (m_document && !m_horizontal_scrolling) {
        m_clip->set_frame(clip);
        m_document->set_frame({ 0, 0, clip.width, m_document->frame().height });
    }
    m_clip->set_frame(clip);
    auto wanted = document_size();
    bool need_vertical = wanted.height > clip.height;
    bool need_horizontal = m_horizontal_scrolling && wanted.width > clip.width;
    if (legacy) {
        if (need_vertical)
            clip.width -= across;
        if (need_horizontal)
            clip.height -= across;
        m_clip->set_frame(clip);
        wanted = document_size();
    }
    if (m_document) {
        float document_width = m_horizontal_scrolling ? wanted.width : clip.width;
        m_document->set_frame({ 0, 0, document_width, wanted.height });
    }
    m_vertical->set_hidden(!need_vertical);
    m_horizontal->set_hidden(!need_horizontal);
    m_vertical->set_frame({ width() - across, m_top_inset, across, height() - m_top_inset - (need_horizontal && legacy ? across : 0) });
    m_horizontal->set_frame({ 0, height() - across, width() - (need_vertical && legacy ? across : 0), across });
    auto largest = largest_offset();
    m_offset = { clamp(m_offset.x, 0.0f, largest.x), clamp(m_offset.y, 0.0f, largest.y) };
    apply_offset();
}

void ScrollView::scroll_on_layer_for_a_while()
{
    if (m_clip->bounds_origin() == m_offset || !window() || !window()->backend().supports_layers())
        return;
    m_clip->set_scrolls_on_layer(true);
    m_clip->set_wants_layer(true);
    // Back in with the rest of the window once it has stopped: it needs no memory of its own for sitting there.
    if (m_layer_timer >= 0)
        Platform::the().cancel_timer(m_layer_timer);
    m_layer_timer = Platform::the().add_timer(500, false, [this] {
        m_layer_timer = -1;
        m_clip->set_wants_layer(false);
    });
}

void ScrollView::apply_offset()
{
    scroll_on_layer_for_a_while();
    m_clip->set_bounds_origin(m_offset);
    update_scrollers();
    if (on_scroll)
        on_scroll();
}

void ScrollView::update_scrollers()
{
    auto wanted = document_size();
    auto clip = m_clip->frame().size();
    auto largest = largest_offset();
    float y = largest.y > 0 ? m_offset.y / largest.y : 0;
    float x = largest.x > 0 ? m_offset.x / largest.x : 0;
    m_vertical->set_knob(y, wanted.height > 0 ? clip.height / wanted.height : 1);
    m_horizontal->set_knob(x, wanted.width > 0 ? clip.width / wanted.width : 1);
}

void ScrollView::scroll_to(FloatPoint point, bool animated)
{
    auto largest = largest_offset();
    FloatPoint target { clamp(point.x, 0.0f, largest.x), clamp(point.y, 0.0f, largest.y) };
    stop_motion();
    if (animated && motion_enabled()) {
        auto from = m_offset;
        (void)animate(Animation::Timing::ease_out(tokens().metric("animation.scrollSeconds"_sv)), [this, from, target](float progress) {
            m_offset = { from.x + (target.x - from.x) * progress, from.y + (target.y - from.y) * progress };
            apply_offset();
        });
        return;
    }
    m_offset = target;
    apply_offset();
    m_vertical->flash();
    m_horizontal->flash();
}

void ScrollView::scroll_rect_into_view(FloatRect const& rect)
{
    auto clip = m_clip->frame().size();
    auto target = m_offset;
    if (rect.y < target.y)
        target.y = rect.y;
    else if (rect.bottom() > target.y + clip.height)
        target.y = rect.bottom() - clip.height;
    if (rect.x < target.x)
        target.x = rect.x;
    else if (rect.right() > target.x + clip.width)
        target.x = rect.right() - clip.width;
    if (target != m_offset)
        scroll_to(target);
}

bool ScrollView::scroll_wheel(ScrollEvent const& event)
{
    auto largest = largest_offset();
    if (largest.x <= 0 && largest.y <= 0)
        return false;
    auto const& t = tokens();
    float resistance = t.metric("scroll.rubberBandResistance"_sv);

    auto moved = [&](float at, float delta, float high) {
        // Past an end, it gives only so much.
        if ((at < 0 && delta < 0) || (at > high && delta > 0))
            return at + delta * resistance * resistance;
        return at + delta;
    };
    m_offset.x = m_horizontal_scrolling ? moved(m_offset.x, event.delta_x, largest.x) : 0;
    m_offset.y = moved(m_offset.y, event.delta_y, largest.y);

    double now = Platform::the().now();
    double since = max(now - m_last_wheel, 0.008);
    m_last_wheel = now;
    // What the wheel has been doing, carried on when it stops.
    float blend = since < 0.15 ? 0.6f : 0;
    m_velocity.x = m_velocity.x * blend + event.delta_x / static_cast<float>(since) * (1 - blend);
    m_velocity.y = m_velocity.y * blend + event.delta_y / static_cast<float>(since) * (1 - blend);
    apply_offset();
    m_vertical->flash();
    m_horizontal->flash();
    start_motion();
    return true;
}

void ScrollView::start_motion()
{
    if (m_motion_timer >= 0 || !motion_enabled())
        return;
    m_last_step = Platform::the().now();
    m_motion_timer = Platform::the().add_timer(16, true, [this] { step_motion(); });
}

void ScrollView::stop_motion()
{
    if (m_motion_timer >= 0) {
        Platform::the().cancel_timer(m_motion_timer);
        m_motion_timer = -1;
    }
    m_velocity = {};
}

void ScrollView::step_motion()
{
    auto const& t = tokens();
    double now = Platform::the().now();
    auto elapsed = static_cast<float>(now - m_last_step);
    m_last_step = now;
    auto largest = largest_offset();
    float decay = static_cast<float>(Math::exponential(static_cast<double>(-t.metric("scroll.momentumDecay"_sv) * elapsed)));
    float response = t.metric("scroll.rubberBandResponse"_sv);
    // Springs back with a critically damped approach to the end it passed.
    float back = static_cast<float>(Math::exponential(static_cast<double>(-elapsed / max(response / 4, 0.001f))));

    bool wheel_active = now - m_last_wheel < 0.05;
    bool moving = false;
    auto advance = [&](float& at, float& velocity, float high) {
        if (at < 0 || at > high) {
            float end = at < 0 ? 0 : high;
            velocity = 0;
            if (!wheel_active) {
                at = end + (at - end) * back;
                if (at - end < 0.25f && at - end > -0.25f)
                    at = end;
            }
            if (at != end)
                moving = true;
            return;
        }
        if (wheel_active) {
            moving = true;
            return;
        }
        velocity *= decay;
        float speed = velocity < 0 ? -velocity : velocity;
        if (speed < 8) {
            velocity = 0;
            return;
        }
        at += velocity * elapsed;
        moving = true;
    };
    advance(m_offset.x, m_velocity.x, largest.x);
    advance(m_offset.y, m_velocity.y, largest.y);
    if (!m_horizontal_scrolling)
        m_offset.x = 0;
    apply_offset();
    if (!moving) {
        Platform::the().cancel_timer(m_motion_timer);
        m_motion_timer = -1;
        m_velocity = {};
    }
}

}
