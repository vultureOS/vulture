/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Painter.h>
#include <LibUI/Layout.h>
#include <LibUI/View.h>
#include <LibUI/Window.h>

namespace UI {

using namespace Gfx;

View::View() = default;

View::~View()
{
    if (m_window)
        m_window->view_going_away(*this);
}

Window* View::window() const
{
    return m_window;
}

void View::did_join_window(Window* window)
{
    if (m_window && m_window != window)
        m_window->view_going_away(*this);
    m_window = window;
    for (auto& child : m_children)
        child->did_join_window(window);
}

void View::adopt_child(NonnullOwnPtr<View> child)
{
    adopt_child_at(m_children.size(), move(child));
}

void View::adopt_child_at(size_t index, NonnullOwnPtr<View> child)
{
    child->m_parent = this;
    child->did_join_window(m_window);
    m_children.insert(index, move(child));
    set_needs_layout();
    set_needs_display();
}

NonnullOwnPtr<View> View::take_child(View& child)
{
    for (size_t i = 0; i < m_children.size(); ++i) {
        if (m_children[i].ptr() == &child) {
            child.set_needs_display();
            child.did_join_window(nullptr);
            child.m_parent = nullptr;
            auto taken = move(m_children[i]);
            m_children.remove(i);
            set_needs_layout();
            return taken;
        }
    }
    VERIFY_NOT_REACHED();
}

void View::remove_child(View& child)
{
    (void)take_child(child);
}

void View::remove_all_children()
{
    while (!m_children.is_empty())
        remove_child(*m_children.last());
}

void View::bring_child_to_front(View& child)
{
    for (size_t i = 0; i < m_children.size(); ++i) {
        if (m_children[i].ptr() == &child) {
            auto taken = move(m_children[i]);
            m_children.remove(i);
            m_children.append(move(taken));
            child.set_needs_display();
            return;
        }
    }
}

void View::set_frame(FloatRect const& frame)
{
    if (frame == m_frame)
        return;
    bool resized = frame.width != m_frame.width || frame.height != m_frame.height;
    set_needs_display();
    m_frame = frame;
    if (resized) {
        m_needs_layout = true;
        m_layer = nullptr;
        m_layer_dirty = true;
        // The window finds this when it lays out: a resize needs one.
        for (auto* ancestor = m_parent; ancestor; ancestor = ancestor->m_parent) {
            if (ancestor->m_needs_layout)
                break;
            ancestor->m_needs_layout = true;
        }
        if (m_window)
            m_window->schedule_update();
    }
    set_needs_display();
}

void View::set_bounds_origin(FloatPoint origin)
{
    if (origin == m_bounds_origin)
        return;
    m_bounds_origin = origin;
    set_needs_display();
}

FloatPoint View::to_window(FloatPoint point) const
{
    for (auto const* view = this; view; view = view->m_parent) {
        point = point - view->m_bounds_origin;
        point = point + view->m_frame.origin() + view->m_translation;
    }
    return point;
}

FloatPoint View::from_window(FloatPoint point) const
{
    auto origin = to_window({ 0, 0 });
    return point - origin;
}

FloatRect View::rect_in_window() const
{
    auto origin = to_window(m_bounds_origin);
    return { origin.x, origin.y, m_frame.width, m_frame.height };
}

void View::set_hidden(bool hidden)
{
    if (hidden == m_hidden)
        return;
    // Before it goes: the area it left must be drawn too.
    if (hidden) {
        set_needs_display();
        if (m_window)
            m_window->view_hidden(*this);
    }
    m_hidden = hidden;
    if (m_parent)
        m_parent->set_needs_layout();
    set_needs_display();
}

bool View::is_visible() const
{
    for (auto const* view = this; view; view = view->m_parent) {
        if (view->m_hidden)
            return false;
    }
    return true;
}

void View::set_opacity(float opacity)
{
    if (opacity == m_opacity)
        return;
    m_opacity = opacity;
    damage_without_redrawing_layer();
}

void View::set_translation(FloatPoint translation)
{
    if (translation == m_translation)
        return;
    damage_without_redrawing_layer();
    m_translation = translation;
    damage_without_redrawing_layer();
}

void View::damage_without_redrawing_layer()
{
    bool was_dirty = m_layer_dirty;
    set_needs_display();
    m_layer_dirty = was_dirty;
}

void View::set_layer_backed(bool backed)
{
    m_layer_backed = backed;
    m_layer = nullptr;
    m_layer_dirty = true;
    set_needs_display();
}

void View::invalidate_ancestor_layers()
{
    for (auto* view = m_parent; view; view = view->m_parent) {
        if (view->m_layer_backed)
            view->m_layer_dirty = true;
    }
}

void View::set_needs_display()
{
    auto spill = overflow();
    set_needs_display({ m_bounds_origin.x - spill.left, m_bounds_origin.y - spill.top, m_frame.width + spill.left + spill.right, m_frame.height + spill.top + spill.bottom });
}

void View::set_needs_display(FloatRect const& rect)
{
    if (m_layer_backed)
        m_layer_dirty = true;
    invalidate_ancestor_layers();
    if (!m_window)
        return;
    // Into the window's terms, step by step, cut by whatever clips on the way.
    auto area = rect;
    for (auto const* view = this; view; view = view->m_parent) {
        if (view->m_hidden)
            return;
        area = area.translated(-view->m_bounds_origin.x, -view->m_bounds_origin.y);
        if (view->m_clips_children && view != this) {
            area = area.intersected({ 0, 0, view->m_frame.width, view->m_frame.height });
            if (area.is_empty())
                return;
        }
        area = area.translated(view->m_frame.x + view->m_translation.x, view->m_frame.y + view->m_translation.y);
    }
    m_window->add_damage(area);
}

void View::set_layout(OwnPtr<Layout> layout)
{
    m_layout = move(layout);
    set_needs_layout();
}

void View::set_needs_layout()
{
    if (m_needs_layout && (!m_window || m_window->update_scheduled()))
        return;
    for (auto* view = this; view; view = view->m_parent)
        view->m_needs_layout = true;
    if (m_window)
        m_window->schedule_update();
}

void View::layout_if_needed()
{
    if (m_needs_layout) {
        m_needs_layout = false;
        layout();
    }
    for (auto& child : m_children) {
        if (!child->m_hidden)
            child->layout_if_needed();
    }
}

void View::layout()
{
    if (m_layout)
        m_layout->apply(*this);
}

FloatSize View::preferred_size() const
{
    FloatSize size = intrinsic_size();
    if ((size.width < 0 || size.height < 0) && m_layout) {
        auto from_layout = m_layout->preferred_size(*this);
        if (size.width < 0)
            size.width = from_layout.width;
        if (size.height < 0)
            size.height = from_layout.height;
    }
    if (m_fixed.width >= 0)
        size.width = m_fixed.width;
    if (m_fixed.height >= 0)
        size.height = m_fixed.height;
    return size;
}

void View::set_fixed_width(float width)
{
    m_fixed.width = width;
    if (m_parent)
        m_parent->set_needs_layout();
}

void View::set_fixed_height(float height)
{
    m_fixed.height = height;
    if (m_parent)
        m_parent->set_needs_layout();
}

View* View::hit_test(FloatPoint in_parent)
{
    if (m_hidden)
        return nullptr;
    auto point = in_parent - m_frame.origin() - m_translation + m_bounds_origin;
    bool inside = bounds().contains(point);
    if (!inside && m_clips_children)
        return nullptr;
    for (size_t i = m_children.size(); i > 0; --i) {
        if (auto* found = m_children[i - 1]->hit_test(point))
            return found;
    }
    return inside ? this : nullptr;
}

Responder* View::next_responder() const
{
    if (m_parent)
        return m_parent;
    return m_window ? static_cast<Responder*>(m_window) : nullptr;
}

bool View::is_first_responder() const
{
    return m_window && m_window->first_responder() == this;
}

bool View::become_first_responder()
{
    return m_window && m_window->make_first_responder(this);
}

void View::set_enabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    set_needs_display();
}

bool View::is_enabled() const
{
    for (auto const* view = this; view; view = view->m_parent) {
        if (!view->m_enabled)
            return false;
    }
    return true;
}

String View::accessibility_label() const
{
    return MUST(String::from_view(m_accessibility_label.view()));
}

String View::accessibility_value() const
{
    return {};
}

void View::set_accessibility_label(StringView label)
{
    m_accessibility_label = MUST(String::from_view(label));
}

void View::set_tooltip(StringView tooltip)
{
    m_tooltip = MUST(String::from_view(tooltip));
}

void View::propagate_appearance_change()
{
    appearance_changed();
    m_layer_dirty = true;
    for (auto& child : m_children)
        child->propagate_appearance_change();
}

void View::cancel_layer()
{
    m_layer = nullptr;
    m_layer_dirty = true;
}

void View::draw_contents(Graphics& graphics, FloatRect const& dirty)
{
    draw(graphics);
    graphics.save();
    if (m_clips_children)
        graphics.clip({ 0, 0, m_frame.width, m_frame.height });
    graphics.translate(-m_bounds_origin.x, -m_bounds_origin.y);
    auto inner_dirty = dirty.translated(m_bounds_origin.x, m_bounds_origin.y);
    for (auto& child : m_children)
        child->draw_tree(graphics, inner_dirty);
    graphics.restore();
    draw_over_children(graphics);
    if (is_first_responder() && shows_focus_ring() && m_window && m_window->shows_focus())
        m_window->draw_focus_ring(graphics, *this);
}

void View::draw_tree(Graphics& graphics, FloatRect const& dirty_in_parent)
{
    if (m_hidden || m_opacity <= 0)
        return;
    auto spill = overflow();
    FloatRect reach { m_frame.x + m_translation.x - spill.left, m_frame.y + m_translation.y - spill.top, m_frame.width + spill.left + spill.right, m_frame.height + spill.top + spill.bottom };
    // A focus ring reaches beyond any overflow.
    if (is_first_responder())
        reach = reach.inflated(4);
    if (!reach.intersects(dirty_in_parent))
        return;

    auto dirty = dirty_in_parent.translated(-m_frame.x - m_translation.x, -m_frame.y - m_translation.y);
    graphics.save();
    graphics.translate(m_frame.x + m_translation.x, m_frame.y + m_translation.y);

    if (!m_layer_backed && m_opacity >= 1) {
        draw_contents(graphics, dirty);
        graphics.restore();
        return;
    }

    // Through a picture of itself.
    float scale = graphics.scale();
    int pad_left = static_cast<int>(spill.left * scale + 0.999f);
    int pad_top = static_cast<int>(spill.top * scale + 0.999f);
    IntSize pixels { static_cast<int>(m_frame.width * scale + 0.999f) + pad_left + static_cast<int>(spill.right * scale + 0.999f),
        static_cast<int>(m_frame.height * scale + 0.999f) + pad_top + static_cast<int>(spill.bottom * scale + 0.999f) };
    if (pixels.is_empty()) {
        graphics.restore();
        return;
    }
    if (!m_layer || m_layer->size() != pixels || m_layer_dirty) {
        if (!m_layer || m_layer->size() != pixels) {
            auto bitmap = Bitmap::create(pixels);
            if (bitmap.is_error()) {
                graphics.restore();
                return;
            }
            m_layer = bitmap.release_value();
        } else {
            m_layer->clear();
        }
        Painter painter(*m_layer);
        painter.scale(scale, scale);
        painter.translate(static_cast<float>(pad_left) / scale, static_cast<float>(pad_top) / scale);
        Graphics layer_graphics(painter, scale);
        // All of it: the picture is used again for any part.
        draw_contents(layer_graphics, { -spill.left, -spill.top, m_frame.width + spill.left + spill.right, m_frame.height + spill.top + spill.bottom });
        m_layer_dirty = false;
    }
    graphics.painter().draw_bitmap(*m_layer, FloatRect { static_cast<float>(-pad_left) / scale, static_cast<float>(-pad_top) / scale, static_cast<float>(pixels.width) / scale, static_cast<float>(pixels.height) / scale }, m_opacity);
    if (!m_layer_backed)
        m_layer = nullptr;
    graphics.restore();
}

}
