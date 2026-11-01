/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Painter.h>
#include <LibUI/Appearance.h>
#include <LibUI/Layout.h>
#include <LibUI/Window.h>
#include <LibUI/WindowRegistry.h>

namespace UI {

using namespace Gfx;

namespace {

Vector<Window*> s_windows;

View* dynamic_view_of(Responder* responder)
{
    return responder ? responder->as_view() : nullptr;
}

class PictureView final : public View {
public:
    explicit PictureView(Function<void(Graphics&)> draw_picture)
        : m_draw_picture(move(draw_picture))
    {
    }

    void draw(Graphics& graphics) override { m_draw_picture(graphics); }

private:
    Function<void(Graphics&)> m_draw_picture;
};

class TooltipView final : public View {
public:
    explicit TooltipView(StringView text)
        : m_text(MUST(String::from_view(text)))
    {
    }

    void draw(Graphics& graphics) override
    {
        auto const& appearance = Appearance::the();
        FloatRect shape { 0, 0, width(), height() };
        graphics.fill_rounded(shape, appearance.metric("tooltip.radius"_sv), appearance.color("tooltip.background"_sv));
        graphics.draw_text_centered(m_text.view(), appearance.named_font("toolTips"_sv), shape, appearance.color("label"_sv));
    }

private:
    String m_text;
};

}

void WindowRegistry::add(Window& window)
{
    s_windows.append(&window);
}

void WindowRegistry::remove(Window& window)
{
    s_windows.remove_all_matching([&](auto* other) { return other == &window; });
}

Span<Window* const> WindowRegistry::windows()
{
    return s_windows.span();
}

void WindowRegistry::appearance_changed()
{
    for (auto* window : s_windows)
        window->handle_appearance_changed();
}

class Window::RootView final : public View {
public:
    explicit RootView(Window& window)
        : m_window(window)
    {
    }

    void draw(Graphics& graphics) override
    {
        if (!m_window.is_transparent())
            graphics.fill({ 0, 0, width(), height() }, Appearance::the().color("windowBackground"_sv));
    }

    void layout() override
    {
        auto title_bar = m_window.title_bar_height();
        m_window.content_view().set_frame({ 0, title_bar, width(), height() - title_bar });
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }

private:
    Window& m_window;
};

// Fills itself with its one child, or arranges them by its layout.
class ContentView final : public View {
public:
    void layout() override
    {
        if (layout_manager()) {
            View::layout();
            return;
        }
        for (auto const& child : children())
            child->set_frame({ 0, 0, width(), height() });
    }
};

Window::Window(StringView title, IntRect frame, u32 flags)
    : m_title(MUST(String::from_view(title)))
    , m_flags(flags)
{
    m_backend = MUST(Platform::the().create_window(*this, title, frame, flags | WS::WindowHidden));
    auto root = adopt_own(*new RootView(*this));
    root->did_join_window(this);
    m_root = move(root);
    m_content = &m_root->add<ContentView>();
    auto size = frame.size().to<float>();
    m_root->set_frame({ 0, 0, size.width, size.height });
    WindowRegistry::add(*this);
}

Window::~Window()
{
    hide_tooltip();
    WindowRegistry::remove(*this);
    m_first_responder = nullptr;
    m_mouse_target = nullptr;
    m_hovered = nullptr;
    m_drop_target = nullptr;
    // Children ask their window about themselves as they go.
    m_root = nullptr;
}

float Window::title_bar_height() const
{
    if ((m_flags & WS::WindowTitled) && !(m_flags & WS::WindowUnifiedTitle) && !(m_flags & WS::WindowBare))
        return Appearance::the().metric("window.titleBarHeight"_sv);
    return 0;
}

void Window::show()
{
    if (m_shown)
        return;
    m_shown = true;
    schedule_update();
    add_damage(m_root->frame());
    update_now();
    m_backend->show(true);
    if (!(m_flags & (WS::WindowPopup | WS::WindowBare)))
        m_backend->activate();
}

void Window::hide()
{
    if (!m_shown)
        return;
    m_shown = false;
    hide_tooltip();
    m_backend->show(false);
}

void Window::close()
{
    if (m_closed)
        return;
    if (on_close_request && !on_close_request())
        return;
    m_closed = true;
    hide();
    if (on_closed)
        on_closed();
}

void Window::set_title(StringView title)
{
    m_title = MUST(String::from_view(title));
    m_backend->set_title(title);
}

IntRect Window::frame() const
{
    return m_backend->frame();
}

void Window::set_frame(IntRect frame)
{
    auto old = m_backend->frame();
    m_backend->set_frame(frame);
    if (old.size() != frame.size()) {
        m_presenting = false;
        handle_resized(frame.size().to<float>());
    }
}

void Window::center_on_screen()
{
    auto screen = Platform::the().screen_size();
    auto current = frame();
    // A little above the middle, where macOS puts them.
    set_frame({ (screen.width - current.width) / 2, (screen.height - current.height) / 3, current.width, current.height });
}

FloatSize Window::size() const
{
    return m_root->frame().size();
}

void Window::set_transparent(bool transparent)
{
    m_transparent = transparent;
    add_damage(m_root->frame());
}

bool Window::make_first_responder(Responder* responder)
{
    if (responder == m_first_responder)
        return true;
    if (responder && !responder->accepts_first_responder())
        return false;
    if (auto* old_view = dynamic_view_of(m_first_responder))
        add_damage(old_view->rect_in_window().inflated(8));
    auto* old = m_first_responder;
    m_first_responder = responder;
    if (old)
        old->did_resign_first_responder();
    if (responder)
        responder->did_become_first_responder();
    if (auto* new_view = dynamic_view_of(m_first_responder))
        add_damage(new_view->rect_in_window().inflated(8));
    return true;
}

void Window::set_default_action(Function<void()> action)
{
    m_default_action = move(action);
}

void Window::set_cancel_action(Function<void()> action)
{
    m_cancel_action = move(action);
}

void Window::set_key_interceptor(Responder* responder)
{
    m_key_interceptor = responder;
}

void Window::set_pointer_catcher(View* view)
{
    m_pointer_catcher = view;
}

View* Window::view_at(FloatPoint position)
{
    if (auto* hit = m_root->hit_test(position))
        return hit;
    return m_pointer_catcher;
}

void Window::set_focus_scope(View* scope)
{
    m_focus_scope = scope;
}

void Window::update_layout_now()
{
    m_root->layout_if_needed();
}

void Window::set_shows_focus(bool shows)
{
    if (shows == m_shows_focus)
        return;
    m_shows_focus = shows;
    if (auto* view = dynamic_view_of(m_first_responder))
        add_damage(view->rect_in_window().inflated(8));
}

Vector<View*> Window::focusable_views() const
{
    Vector<View*> views;
    auto walk = [&](auto& self, View& view) -> void {
        if (view.is_hidden())
            return;
        if (view.can_take_key_focus())
            views.append(&view);
        for (auto const& child : view.children())
            self(self, *child);
    };
    walk(walk, m_focus_scope ? *m_focus_scope : *m_root);
    return views;
}

void Window::focus_next(bool backward)
{
    auto views = focusable_views();
    if (views.is_empty())
        return;
    size_t index = views.size();
    for (size_t i = 0; i < views.size(); ++i) {
        if (views[i] == m_first_responder)
            index = i;
    }
    size_t next;
    if (index == views.size())
        next = backward ? views.size() - 1 : 0;
    else if (backward)
        next = index == 0 ? views.size() - 1 : index - 1;
    else
        next = index + 1 == views.size() ? 0 : index + 1;
    m_shows_focus = true;
    make_first_responder(views[next]);
}

bool Window::key_down(KeyEvent const& event)
{
    if (event.code == KeyTab && !event.has_command() && !event.has_control()) {
        focus_next(event.has_shift());
        return true;
    }
    if (event.code == KeyEnter && m_default_action) {
        m_default_action();
        return true;
    }
    if (event.code == KeyEscape && m_cancel_action) {
        m_cancel_action();
        return true;
    }
    return false;
}

void Window::add_damage(FloatRect const& rect)
{
    if (rect.is_empty() || !m_root)
        return;
    auto area = rect.intersected({ 0, 0, m_root->width(), m_root->height() });
    if (area.is_empty())
        return;
    // A few rectangles, and joined with any they touch: an alert's two buttons need
    // not redraw all that lies between them.
    for (auto& existing : m_damage) {
        if (existing.intersects(area)) {
            existing = existing.united(area);
            schedule_update();
            return;
        }
    }
    if (m_damage.size() >= 8) {
        for (size_t i = 1; i < m_damage.size(); ++i)
            m_damage[0] = m_damage[0].united(m_damage[i]);
        m_damage[0] = m_damage[0].united(area);
        while (m_damage.size() > 1)
            (void)m_damage.take_last();
    } else {
        m_damage.append(area);
    }
    schedule_update();
}

void Window::schedule_update()
{
    if (m_update_scheduled)
        return;
    m_update_scheduled = true;
    Platform::the().defer([this] {
        // (It may have gone since: windows are told about by the registry.)
        for (auto* window : WindowRegistry::windows()) {
            if (window == this) {
                m_update_scheduled = false;
                update_now();
                return;
            }
        }
    });
}

void Window::update_now()
{
    if (!m_shown && !m_update_scheduled && m_damage.is_empty())
        return;
    if (m_presenting) {
        m_update_pending = true;
        return;
    }
    m_root->layout_if_needed();
    if (m_damage.is_empty())
        return;

    auto& bitmap = m_backend->back_buffer();
    float scale = static_cast<float>(m_backend->scale());
    IntRect total;
    for (auto const& area : m_damage) {
        // To the pixels it touches, and back: antialiased edges never straddle.
        auto pixels = FloatRect { area.x * scale, area.y * scale, area.width * scale, area.height * scale }.enclosing_int_rect().intersected(bitmap.rect());
        if (pixels.is_empty())
            continue;
        total = total.united(pixels);
        for (int y = pixels.y; y < pixels.bottom(); ++y) {
            auto* row = bitmap.scanline(y);
            for (int x = pixels.x; x < pixels.right(); ++x)
                row[x] = 0;
        }
        Painter painter(bitmap);
        painter.clip_rect(pixels.to<float>());
        painter.scale(scale, scale);
        Graphics graphics(painter, scale);
        FloatRect dirty { static_cast<float>(pixels.x) / scale, static_cast<float>(pixels.y) / scale, static_cast<float>(pixels.width) / scale, static_cast<float>(pixels.height) / scale };
        m_root->draw_tree(graphics, dirty);
    }
    m_damage.clear();
    if (total.is_empty())
        return;
    m_presenting = true;
    m_backend->present(total);
}

void Window::handle_presented()
{
    m_presenting = false;
    if (m_update_pending) {
        m_update_pending = false;
        update_now();
    }
}

void Window::draw_focus_ring(Graphics& graphics, View& view)
{
    auto const& appearance = Appearance::the();
    float width = appearance.metric("focus.width"_sv);
    auto rect = view.focus_ring_rect();
    graphics.stroke_rounded(rect.inflated(width), view.focus_ring_radius() + width, width, appearance.color("focus"_sv));
}

void Window::set_materials(Span<MaterialRegion const> regions)
{
    m_backend->set_materials(regions);
}

void Window::forget_press()
{
    m_mouse_target = nullptr;
    m_buttons_down = 0;
}

void Window::move_by_pointer()
{
    forget_press();
    m_backend->begin_move();
}

void Window::begin_drag(StringView type, ReadonlyBytes data, FloatSize size, Function<void(Graphics&)> draw)
{
    forget_press();
    IntRect image_frame { static_cast<int>(m_pointer.x), static_cast<int>(m_pointer.y), static_cast<int>(size.width + 0.999f), static_cast<int>(size.height + 0.999f) };
    m_drag_image = adopt_own(*new Window("drag"_sv, image_frame, WS::WindowBare | WS::WindowPopup));
    m_drag_image->set_transparent(true);
    m_drag_image->root_view().add<PictureView>(move(draw)).set_frame({ 0, 0, size.width, size.height });
    m_drag_image->show();
    m_backend->begin_drag(m_drag_image->backend(), type, data);
}

void Window::handle_drag(WS::DragPhase phase, FloatPoint position, StringView type, ReadonlyBytes data)
{
    auto target_at = [&](FloatPoint point) -> View* {
        for (auto* view = m_root->hit_test(point); view; view = view->parent()) {
            if (view->is_enabled() && view->accepts_drop(type))
                return view;
        }
        return nullptr;
    };
    auto set_target = [&](View* target) {
        if (target == m_drop_target)
            return;
        if (m_drop_target)
            m_drop_target->drag_exited();
        m_drop_target = target;
        if (m_drop_target)
            m_drop_target->drag_entered();
        m_backend->accept_drop(m_drop_target != nullptr);
    };
    switch (phase) {
    case WS::DragPhase::Entered:
    case WS::DragPhase::Moved:
        set_target(target_at(position));
        break;
    case WS::DragPhase::Left:
        set_target(nullptr);
        break;
    case WS::DragPhase::Dropped:
        if (auto* target = target_at(position))
            target->perform_drop(type, data);
        set_target(nullptr);
        break;
    }
}

void Window::handle_drag_finished(bool accepted)
{
    m_drag_image = nullptr;
    if (on_drag_finished)
        on_drag_finished(accepted);
}

void Window::set_hovered(View* view)
{
    if (view == m_hovered)
        return;
    auto contains = [](View* ancestor, View* of) {
        for (auto* walk = of; walk; walk = walk->parent()) {
            if (walk == ancestor)
                return true;
        }
        return false;
    };
    auto* old = m_hovered;
    m_hovered = view;
    // Out of those it has left, then into those it has reached, outermost first.
    for (auto* walk = old; walk && !contains(walk, view); walk = walk->parent())
        walk->mouse_exited();
    Vector<View*> entered;
    for (auto* walk = view; walk && !contains(walk, old); walk = walk->parent())
        entered.append(walk);
    for (size_t i = entered.size(); i > 0; --i)
        entered[i - 1]->mouse_entered();
    hide_tooltip();
    arm_tooltip();
}

void Window::update_cursor()
{
    auto shape = m_hovered ? m_hovered->cursor() : WS::CursorShape::Arrow;
    if (m_mouse_target)
        shape = m_mouse_target->cursor();
    if (shape == m_cursor)
        return;
    m_cursor = shape;
    m_backend->set_cursor(shape);
}

void Window::arm_tooltip()
{
    if (!m_hovered || m_hovered->tooltip().is_empty())
        return;
    auto delay = static_cast<u64>(Appearance::the().metric("tooltip.delayMilliseconds"_sv));
    m_tooltip_timer = Platform::the().add_timer(delay, false, [this] {
        m_tooltip_timer = -1;
        if (!m_hovered || m_hovered->tooltip().is_empty())
            return;
        auto const& appearance = Appearance::the();
        auto text = m_hovered->tooltip();
        auto font = appearance.named_font("toolTips"_sv);
        int width = static_cast<int>(Appearance::text_width(font, text) + appearance.metric("tooltip.padding"_sv) * 2 + 0.999f);
        int height = static_cast<int>(appearance.metric("tooltip.height"_sv));
        auto origin = frame().origin();
        IntRect place { origin.x + static_cast<int>(m_pointer.x), origin.y + static_cast<int>(m_pointer.y) + 20, width, height };
        m_tooltip = adopt_own(*new Window("tooltip"_sv, place, WS::WindowBare | WS::WindowPopup));
        m_tooltip->set_transparent(true);
        auto& view = m_tooltip->root_view().add<TooltipView>(text);
        view.set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
        m_tooltip->show();
    });
}

void Window::hide_tooltip()
{
    if (m_tooltip_timer >= 0) {
        Platform::the().cancel_timer(m_tooltip_timer);
        m_tooltip_timer = -1;
    }
    m_tooltip = nullptr;
}

void Window::handle_mouse(WS::MouseKind kind, FloatPoint position, u32 buttons, u32 modifiers, int wheel_x, int wheel_y)
{
    m_pointer = position;
    auto make_event = [&](View& view, u32 button) {
        MouseEvent event;
        event.position = view.from_window(position);
        event.window_position = position;
        event.buttons = buttons;
        event.button = button;
        event.modifiers = modifiers;
        event.click_count = m_click_count;
        return event;
    };

    switch (kind) {
    case WS::MouseKind::Moved: {
        if (m_mouse_target) {
            m_mouse_target->mouse_dragged(make_event(*m_mouse_target, 0));
            update_cursor();
            return;
        }
        auto* hit = view_at(position);
        set_hovered(hit);
        update_cursor();
        for (auto* view = hit; view; view = view->parent())
            view->mouse_moved(make_event(*view, 0));
        return;
    }
    case WS::MouseKind::Pressed: {
        hide_tooltip();
        u32 button = buttons & ~m_buttons_down;
        m_buttons_down = buttons;
        if (button == 0)
            button = WS::ButtonLeft;
        double now = Platform::the().now();
        auto const& appearance = Appearance::the();
        float near = appearance.metric("mouse.doubleClickDistance"_sv);
        auto moved = position - m_last_press_position;
        bool same_click = button == m_last_press_button && now - m_last_press_time <= static_cast<double>(appearance.metric("mouse.doubleClickSeconds"_sv))
            && moved.x * moved.x + moved.y * moved.y <= near * near;
        m_click_count = same_click ? m_click_count + 1 : 1;
        m_last_press_time = now;
        m_last_press_position = position;
        m_last_press_button = button;

        // Only one view tracks a press.
        if (m_mouse_target)
            return;
        auto* hit = view_at(position);
        set_hovered(hit);
        for (auto* view = hit; view; view = view->parent()) {
            if (!view->is_enabled())
                continue;
            bool had_press = m_buttons_down != 0;
            bool taken = view->mouse_down(make_event(*view, button));
            // (It may have handed the press on: see forget_press.)
            if (taken && had_press && m_buttons_down != 0)
                m_mouse_target = view;
            if (taken)
                break;
        }
        // A click in a control that takes the keyboard gives it the keyboard.
        for (auto* view = hit; view; view = view->parent()) {
            if (view->accepts_first_responder() && view->is_enabled()) {
                m_shows_focus = true;
                make_first_responder(view);
                break;
            }
        }
        update_cursor();
        return;
    }
    case WS::MouseKind::Released: {
        u32 button = m_buttons_down & ~buttons;
        m_buttons_down = buttons;
        if (auto* target = m_mouse_target) {
            if (buttons == 0)
                m_mouse_target = nullptr;
            target->mouse_up(make_event(*target, button));
        } else if (m_pointer_catcher) {
            // A press that began before this window took the pointer: a menu's.
            m_pointer_catcher->mouse_up(make_event(*m_pointer_catcher, button));
        }
        auto* hit = view_at(position);
        set_hovered(hit);
        update_cursor();
        return;
    }
    case WS::MouseKind::Wheel: {
        auto const step = Appearance::the().metric("scroll.pointsPerNotch"_sv);
        auto* hit = m_root->hit_test(position);
        for (auto* view = hit; view; view = view->parent()) {
            ScrollEvent event;
            event.position = view->from_window(position);
            event.delta_x = static_cast<float>(wheel_x) * step;
            event.delta_y = static_cast<float>(wheel_y) * step;
            event.modifiers = modifiers;
            if (view->is_enabled() && view->scroll_wheel(event))
                return;
        }
        return;
    }
    case WS::MouseKind::Entered: {
        // It came in somewhere: as good as moving there.
        auto* hit = view_at(position);
        set_hovered(hit);
        update_cursor();
        for (auto* view = hit; view; view = view->parent())
            view->mouse_moved(make_event(*view, 0));
        return;
    }
    case WS::MouseKind::Left:
        hide_tooltip();
        set_hovered(nullptr);
        return;
    }
}

void Window::handle_key(bool pressed, u16 code, u32 modifiers, StringView text)
{
    hide_tooltip();
    KeyEvent event;
    event.code = code;
    event.modifiers = modifiers;
    event.text = text;
    if (m_key_interceptor) {
        if (pressed ? m_key_interceptor->key_down(event) : m_key_interceptor->key_up(event))
            return;
    }
    for (Responder* responder = m_first_responder ? m_first_responder : static_cast<Responder*>(this); responder; responder = responder->next_responder()) {
        if (pressed ? responder->key_down(event) : responder->key_up(event))
            return;
    }
}

void Window::handle_resized(FloatSize size)
{
    m_presenting = false;
    m_root->set_frame({ 0, 0, size.width, size.height });
    // Fresh pixels: all of it.
    add_damage({ 0, 0, size.width, size.height });
    if (on_resize)
        on_resize(size);
}

void Window::handle_activation(bool active)
{
    if (active == m_key)
        return;
    m_key = active;
    // Controls draw differently in a window in the background.
    add_damage({ 0, 0, m_root->width(), m_root->height() });
    if (on_key_change)
        on_key_change(active);
}

void Window::handle_close_request()
{
    close();
}

void Window::handle_appearance_changed()
{
    m_root->propagate_appearance_change();
    add_damage({ 0, 0, m_root->width(), m_root->height() });
}

void Window::view_going_away(View& view)
{
    if (m_first_responder == &view)
        m_first_responder = nullptr;
    if (m_key_interceptor == &view)
        m_key_interceptor = nullptr;
    if (m_focus_scope == &view)
        m_focus_scope = nullptr;
    if (m_pointer_catcher == &view)
        m_pointer_catcher = nullptr;
    if (m_mouse_target == &view)
        m_mouse_target = nullptr;
    if (m_hovered == &view)
        m_hovered = nullptr;
    if (m_drop_target == &view)
        m_drop_target = nullptr;
}

void Window::view_hidden(View& view)
{
    auto within = [&](View* inner) {
        for (auto* walk = inner; walk; walk = walk->parent()) {
            if (walk == &view)
                return true;
        }
        return false;
    };
    if (within(dynamic_view_of(m_first_responder)))
        make_first_responder(nullptr);
    if (m_mouse_target && within(m_mouse_target))
        m_mouse_target = nullptr;
    if (m_hovered && within(m_hovered))
        set_hovered(nullptr);
}

float Window::click_distance() const
{
    return Appearance::the().metric("mouse.dragDistance"_sv);
}

}
