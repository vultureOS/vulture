/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Painter.h>
#include <LibUI/Appearance.h>
#include <LibUI/Layout.h>
#include <LibUI/ServerLayer.h>
#include <LibUI/Window.h>
#include <LibUI/WindowRegistry.h>

namespace UI {

using namespace Gfx;

namespace {

Vector<Window*> s_windows;
Window* s_main_window = nullptr;

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
    if (s_main_window == &window)
        s_main_window = nullptr;
}

void WindowRegistry::became_key(Window& window)
{
    s_main_window = &window;
}

Window* WindowRegistry::main_window()
{
    return s_main_window && s_main_window->is_shown() ? s_main_window : nullptr;
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
        float const tabs = m_window.has_tab_bar() ? Appearance::the().metric("tabBar.height"_sv) : 0;
        m_window.content_view().set_frame({ 0, title_bar + tabs, width(), height() - title_bar - tabs });
        if (m_tab_bar)
            m_tab_bar->set_frame({ 0, title_bar, width(), tabs });
    }

    void set_tab_bar(View* bar)
    {
        m_tab_bar = bar;
        set_needs_layout();
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }

private:
    View* m_tab_bar { nullptr };
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

// The bar of a window with tabs: one each, of equal width, the one that's showing the color of the title bar.
class WindowTabBar final : public View {
public:
    explicit WindowTabBar(Window& window)
        : m_window(window)
    {
    }

private:
    [[nodiscard]] FloatRect tab_rect(int index) const
    {
        float const each = width() / static_cast<float>(max(m_window.tab_count(), 1));
        return { each * static_cast<float>(index), 0, each, height() };
    }

    [[nodiscard]] FloatRect close_rect(int index) const
    {
        auto const tab = tab_rect(index);
        float const size = Appearance::the().metric("tabBar.closeSize"_sv);
        return { tab.x + 6, (height() - size) / 2, size, size };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = Appearance::the();
        graphics.fill({ 0, 0, width(), height() }, t.color("tabBar.background"_sv));
        auto const font = t.named_font("small"_sv);
        int const count = m_window.tab_count();
        int const selected = m_window.selected_tab();
        for (int i = 0; i < count; ++i) {
            auto const tab = tab_rect(i);
            bool const is_selected = i == selected;
            if (is_selected)
                graphics.fill(tab, t.color("tabBar.selected"_sv));
            else if (i == m_hovered)
                graphics.fill(tab, t.color("tabBar.hover"_sv));
            if (i > 0)
                graphics.fill({ tab.x, 0, graphics.pixel(), height() }, t.color("tabBar.separator"_sv));
            auto const* window = m_window.tab_at(i);
            graphics.draw_text_centered(window ? window->title() : StringView {}, font, FloatRect { tab.x + 24, tab.y, tab.width - 48, tab.height }, is_selected ? t.color("label"_sv) : t.color("secondaryLabel"_sv));
            if (i == m_hovered) {
                auto const close = close_rect(i);
                if (i == m_over_close)
                    graphics.fill_rounded(close, 3, t.color("tabBar.separator"_sv));
                graphics.draw_text_centered("\xC3\x97"_sv, font, close, t.color("secondaryLabel"_sv));
            }
        }
        // The line under the title bar goes on everywhere but over the one that's showing.
        graphics.fill({ 0, height() - graphics.pixel(), width(), graphics.pixel() }, t.color("tabBar.separator"_sv));
        if (selected >= 0) {
            auto const tab = tab_rect(selected);
            graphics.fill({ tab.x, 0, tab.width, graphics.pixel() }, t.color("tabBar.selected"_sv));
        }
    }

    [[nodiscard]] int tab_under(FloatPoint point) const
    {
        for (int i = 0; i < m_window.tab_count(); ++i) {
            if (tab_rect(i).contains(point))
                return i;
        }
        return -1;
    }

    void mouse_moved(MouseEvent const& event) override
    {
        int const hovered = tab_under(event.position);
        int const close = hovered >= 0 && close_rect(hovered).contains(event.position) ? hovered : -1;
        if (hovered == m_hovered && close == m_over_close)
            return;
        m_hovered = hovered;
        m_over_close = close;
        set_needs_display();
    }

    void mouse_exited() override
    {
        m_hovered = -1;
        m_over_close = -1;
        set_needs_display();
    }

    bool mouse_down(MouseEvent const& event) override
    {
        int const index = tab_under(event.position);
        if (index < 0)
            return false;
        if (close_rect(index).contains(event.position) && m_hovered == index)
            m_window.close_tab(index);
        else
            m_window.select_tab(index);
        return true;
    }

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (int i = 0; i < m_window.tab_count(); ++i) {
            auto const* window = m_window.tab_at(i);
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Tab, window ? window->title() : StringView {}, tab_rect(i), i == m_window.selected_tab() ? "selected"_sv : StringView {}));
        }
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::TabGroup; }

    Window& m_window;
    int m_hovered { -1 };
    int m_over_close { -1 };
};

int Window::tab_count() const
{
    return m_tab_group ? static_cast<int>(m_tab_group->windows.size()) : 1;
}

int Window::selected_tab() const
{
    if (!m_tab_group)
        return 0;
    for (size_t i = 0; i < m_tab_group->windows.size(); ++i) {
        if (m_tab_group->windows[i] == this)
            return static_cast<int>(i);
    }
    return 0;
}

Window const* Window::tab_at(int index) const
{
    if (!m_tab_group)
        return this;
    return m_tab_group->windows[static_cast<size_t>(index)];
}

void Window::tabs_changed()
{
    bool const wanted = m_tab_group && m_tab_group->windows.size() > 1;
    auto* root = static_cast<RootView*>(m_root.ptr());
    if (wanted && !m_tab_bar) {
        m_tab_bar = &m_root->add<WindowTabBar>(*this);
        root->set_tab_bar(m_tab_bar);
    } else if (!wanted && m_tab_bar) {
        root->set_tab_bar(nullptr);
        m_root->remove_child(*m_tab_bar);
        m_tab_bar = nullptr;
    }
    m_root->set_needs_layout();
    if (m_tab_bar)
        m_tab_bar->set_needs_display();
    schedule_update();
    add_damage(m_root->frame());
}

void Window::merge_all_windows()
{
    // Those of the same kind: with a title bar that can be resized, and not a sheet or a panel.
    if (!(m_flags & WS::WindowTitled) || !(m_flags & WS::WindowResizable))
        return;
    if (!m_tab_group) {
        m_tab_group = new TabGroup;
        m_tab_group->windows.append(this);
    }
    for (auto* window : WindowRegistry::windows()) {
        if (window == this || !window->is_shown() || !(window->flags() & WS::WindowTitled) || !(window->flags() & WS::WindowResizable) || window->m_tab_group)
            continue;
        m_tab_group->windows.append(window);
        window->m_tab_group = m_tab_group;
        window->m_backend->take_place_of(*m_backend);
        window->hide();
    }
    for (auto* window : m_tab_group->windows)
        window->tabs_changed();
}

void Window::add_tab(Window& other)
{
    if (&other == this || other.m_tab_group || !(m_flags & WS::WindowTitled) || !(other.flags() & WS::WindowTitled))
        return;
    if (!m_tab_group) {
        m_tab_group = new TabGroup;
        m_tab_group->windows.append(this);
    }
    m_tab_group->windows.append(&other);
    other.m_tab_group = m_tab_group;
    // It's what's in front now: over this, and this goes.
    other.m_backend->take_place_of(*m_backend);
    hide();
    for (auto* window : m_tab_group->windows)
        window->tabs_changed();
}

void Window::select_tab(int index)
{
    if (!m_tab_group || index < 0 || index >= tab_count() || index == selected_tab())
        return;
    auto* chosen = m_tab_group->windows[static_cast<size_t>(index)];
    // Over the old one and then that goes, so there's never nothing there.
    chosen->m_backend->take_place_of(*m_backend);
    chosen->show();
    hide();
}

void Window::show_adjacent_tab(int by)
{
    if (!m_tab_group)
        return;
    int const count = tab_count();
    select_tab((selected_tab() + by + count) % count);
}

void Window::close_tab(int index)
{
    if (!m_tab_group || index < 0 || index >= tab_count())
        return;
    m_tab_group->windows[static_cast<size_t>(index)]->close();
}

void Window::move_tab_to_new_window()
{
    if (!m_tab_group || tab_count() < 2)
        return;
    auto frame = this->frame();
    int const index = selected_tab();
    auto* group = m_tab_group;
    int const count = tab_count();
    auto* next = group->windows[static_cast<size_t>((index + 1) % count)];
    next->m_backend->take_place_of(*m_backend);
    next->show();
    group->windows.remove(static_cast<size_t>(index));
    m_tab_group = nullptr;
    for (auto* window : group->windows)
        window->tabs_changed();
    if (group->windows.size() == 1) {
        group->windows[0]->m_tab_group = nullptr;
        group->windows[0]->tabs_changed();
        delete group;
    }
    tabs_changed();
    // A little down and across, to be seen as another.
    frame.x += 24;
    frame.y += 24;
    set_frame(frame);
    m_backend->activate();
}

void Window::leave_tab_group()
{
    if (!m_tab_group)
        return;
    auto* group = m_tab_group;
    int const index = selected_tab();
    bool const was_showing = is_shown();
    int const count = tab_count();
    m_tab_group = nullptr;
    if (was_showing && count > 1) {
        auto* next = group->windows[static_cast<size_t>(index + 1 < count ? index + 1 : index - 1)];
        next->m_backend->take_place_of(*m_backend);
        next->show();
    }
    group->windows.remove(static_cast<size_t>(index));
    for (auto* window : group->windows)
        window->tabs_changed();
    if (group->windows.size() <= 1) {
        if (!group->windows.is_empty()) {
            group->windows[0]->m_tab_group = nullptr;
            group->windows[0]->tabs_changed();
        }
        delete group;
    }
}

Window::~Window()
{
    leave_tab_group();
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
    if (!(m_flags & (WS::WindowPopup | WS::WindowBare)))
        keep_on_screen();
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
    leave_tab_group();
    hide();
    // The window server has nothing more to say to it: it isn't key, and there's nothing left to draw that shows it.
    m_key = false;
    if (on_closed)
        on_closed();
    if (on_discard)
        on_discard();
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

// A window made for a bigger screen than this one is moved, and only if that isn't enough, made smaller:
// all of it stays where it can be reached, below the menu bar.
void Window::keep_on_screen()
{
    auto const screen = Platform::the().screen_size();
    if (screen.width <= 0 || screen.height <= 0)
        return;
    auto wanted = frame();
    int const top = static_cast<int>(Appearance::the().metric("menubar.height"_sv));
    wanted.width = min(wanted.width, screen.width);
    wanted.height = min(wanted.height, screen.height - top);
    wanted.x = max(0, min(wanted.x, screen.width - wanted.width));
    wanted.y = max(top, min(wanted.y, screen.height - wanted.height));
    if (wanted != frame())
        set_frame(wanted);
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
    update_layers();

    bool has_layer_damage = false;
    for (auto* view : m_layer_views) {
        if (!view->m_server_layer->damage.is_empty() || view->m_server_layer->present_all)
            has_layer_damage = true;
    }
    if (m_damage.is_empty() && !has_layer_damage && !m_backend->has_layer_changes())
        return;

    IntRect total;
    if (!m_damage.is_empty()) {
        auto& bitmap = m_backend->back_buffer();
        float scale = static_cast<float>(m_backend->scale());
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
    }
    for (auto* view : m_layer_views)
        draw_layer(*view);
    send_queued_layer_animations();

    if (!m_backend->has_layer_changes()) {
        if (total.is_empty())
            return;
        m_presenting = true;
        m_backend->present(total);
        return;
    }
    if (!total.is_empty())
        m_backend->layer_present_window(total);
    m_presenting = true;
    m_backend->layer_commit();
}

// ---- Layers

void Window::layers_need_planning()
{
    m_layer_plan_stale = true;
    schedule_update();
}

void Window::layers_may_need_planning()
{
    if (!m_layer_views.is_empty())
        layers_need_planning();
}

void Window::add_layer_damage(View& view, FloatRect const& area)
{
    auto& damage = view.m_server_layer->damage;
    if (damage.size() >= 8) {
        for (size_t i = 1; i < damage.size(); ++i)
            damage[0] = damage[0].united(damage[i]);
        damage[0] = damage[0].united(area);
        while (damage.size() > 1)
            (void)damage.take_last();
    } else {
        damage.append(area);
    }
    schedule_update();
}

void Window::queue_layer_animation(View& view, WS::LayerProperty property, Span<float const> from, Span<float const> to, Animation::Timing const& timing, bool repeats, int steps, Function<void()> finished)
{
    QueuedLayerAnimation queued { &view, property, {}, {}, timing, repeats, steps, 0 };
    for (size_t i = 0; i < 4; ++i) {
        queued.from[i] = i < from.size() ? from[i] : 0;
        queued.to[i] = i < to.size() ? to[i] : 0;
    }
    if (finished) {
        queued.tag = m_next_animation_tag++;
        m_layer_animation_endings.append(LayerAnimationEnding { queued.tag, &view, move(finished) });
    }
    m_queued_layer_animations.append(move(queued));
    schedule_update();
}

void Window::send_queued_layer_animations()
{
    for (auto const& queued : m_queued_layer_animations) {
        if (queued.view->m_server_layer)
            m_backend->layer_animate(*queued.view->m_server_layer->backend, queued.property, { queued.from, 4 }, { queued.to, 4 }, queued.timing, queued.tag, queued.repeats, queued.steps);
    }
    m_queued_layer_animations.clear();
}

void Window::handle_layer_animation_finished(u32 tag)
{
    for (size_t i = 0; i < m_layer_animation_endings.size(); ++i) {
        if (m_layer_animation_endings[i].tag != tag)
            continue;
        auto finished = move(m_layer_animation_endings[i].finished);
        m_layer_animation_endings.remove(i);
        if (finished)
            finished();
        return;
    }
}

void Window::plan_layers_of(View& view, View* layer_parent, Vector<FloatRect>& covered, Vector<View*>& plan)
{
    if (view.m_hidden)
        return;
    auto const spill = view.overflow();
    auto const rect = view.rect_in_window();
    FloatRect const reach { rect.x - spill.left, rect.y - spill.top, rect.width + spill.left + spill.right, rect.height + spill.top + spill.bottom };
    bool layered = view.m_wants_layer;
    // What's drawn after something with a layer, and over it, has to go over it on the screen too, and so needs one.
    if (!layered && &view != m_root.ptr()) {
        for (auto const& other : covered) {
            if (other.intersects(reach)) {
                layered = true;
                break;
            }
        }
    }
    if (!layered) {
        // A picture of itself, drawn here, with all that's in it.
        if (view.m_layer_backed || view.m_opacity < 1)
            return;
        for (auto& child : view.m_children)
            plan_layers_of(*child, layer_parent, covered, plan);
        return;
    }
    view.m_planned_layer_parent = layer_parent;
    plan.append(&view);
    // What's in it is over all that it is, so only has itself to think about.
    Vector<FloatRect> inside;
    for (auto& child : view.m_children)
        plan_layers_of(*child, &view, inside, plan);
    covered.append(reach);
}

void Window::drop_layer(View& view)
{
    if (!view.m_server_layer)
        return;
    m_backend->layer_destroy(*view.m_server_layer->backend);
    auto const spill = view.overflow();
    auto const rect = view.rect_in_window();
    add_damage({ rect.x - spill.left - 4, rect.y - spill.top - 4, rect.width + spill.left + spill.right + 8, rect.height + spill.top + spill.bottom + 8 });
    view.m_server_layer = nullptr;
}

void Window::make_layer(View& view, View* parent)
{
    auto backend = m_backend->create_layer(parent ? parent->m_server_layer->backend.ptr() : nullptr);
    if (backend.is_error())
        return;
    view.m_server_layer = adopt_own(*new ServerLayer(backend.release_value(), parent));
    // Off the window's own surface, in the same frame as it comes on here.
    auto const spill = view.overflow();
    auto const rect = view.rect_in_window();
    add_damage({ rect.x - spill.left - 4, rect.y - spill.top - 4, rect.width + spill.left + spill.right + 8, rect.height + spill.top + spill.bottom + 8 });
    view.layer_was_made();
}

void Window::plan_layers()
{
    m_layer_plan_stale = false;
    Vector<View*> plan;
    Vector<FloatRect> covered;
    plan_layers_of(*m_root, nullptr, covered, plan);

    auto const in_plan = [&](View const* view) {
        for (auto const* planned : plan) {
            if (planned == view)
                return true;
        }
        return false;
    };
    // The innermost first. (In the end, what's in something that has gone has gone too, and has to be made again.)
    for (size_t i = m_layer_views.size(); i > 0; --i) {
        auto* view = m_layer_views[i - 1];
        if (!in_plan(view) || !view->m_server_layer || view->m_server_layer->parent != view->m_planned_layer_parent)
            drop_layer(*view);
    }
    for (auto* view : plan) {
        if (!view->m_server_layer)
            make_layer(*view, view->m_planned_layer_parent);
    }
    m_layer_views.clear();
    for (auto* view : plan) {
        if (view->m_server_layer)
            m_layer_views.append(view);
    }
}

void Window::fit_surface(View& view)
{
    auto& layer = *view.m_server_layer;
    float const scale = static_cast<float>(m_backend->scale());
    auto const spill = view.overflow();
    // (A focus ring goes further than anything says it does.)
    float constexpr ring = 4;
    FloatRect const wanted { -spill.left - ring, -spill.top - ring, view.m_frame.width + spill.left + spill.right + 2 * ring, view.m_frame.height + spill.top + spill.bottom + 2 * ring };
    IntSize const pixels { static_cast<int>(Base::Math::ceil(static_cast<double>(wanted.width * scale))), static_cast<int>(Base::Math::ceil(static_cast<double>(wanted.height * scale))) };
    if (pixels == layer.surface_pixels && wanted == layer.surface_rect)
        return;
    layer.surface_rect = wanted;
    if (pixels != layer.surface_pixels && !pixels.is_empty()) {
        if (layer.backend->set_surface(pixels).is_error())
            return;
        layer.surface_pixels = pixels;
    }
    layer.damage.clear();
    layer.damage.append(wanted);
    layer.present_all = true;
}

void Window::fit_scrolling_surface(View& view)
{
    auto& layer = *view.m_server_layer;
    float const scale = static_cast<float>(m_backend->scale());
    float const width = view.m_frame.width;
    float const height = view.m_frame.height;
    // As much as there is to it.
    float document_width = width;
    float document_height = height;
    for (auto const& child : view.m_children) {
        document_width = max(document_width, child->m_frame.right());
        document_height = max(document_height, child->m_frame.bottom());
    }
    // Half as much again each way, in whichever way it can go. That's a good few steps of a wheel, and it's kept ready before it's needed.
    float const more_across = document_width > width ? width / 2 : 0;
    float const more_down = document_height > height ? height / 2 : 0;
    float const wanted_width = min(width + 2 * more_across, document_width);
    float const wanted_height = min(height + 2 * more_down, document_height);
    auto const snapped = [&](float value) { return static_cast<float>(Base::Math::round(static_cast<double>(value * scale))) / scale; };
    auto const place = [&](float origin, float more, float wanted, float document) { return snapped(clamp(origin - more, 0.0f, max(document - wanted, 0.0f))); };
    auto const origin = view.m_bounds_origin;
    FloatRect const centred { place(origin.x, more_across, wanted_width, document_width), place(origin.y, more_down, wanted_height, document_height), wanted_width, wanted_height };
    IntSize const pixels { static_cast<int>(Base::Math::ceil(static_cast<double>(wanted_width * scale))), static_cast<int>(Base::Math::ceil(static_cast<double>(wanted_height * scale))) };
    if (pixels.is_empty())
        return;
    if (pixels != layer.surface_pixels) {
        if (layer.backend->set_surface(pixels).is_error())
            return;
        layer.surface_pixels = pixels;
        layer.surface_rect = centred;
        layer.damage.clear();
        layer.damage.append(centred);
        layer.present_all = true;
        return;
    }
    auto const& held = layer.surface_rect;
    // Close enough to the edge of what's held, with more beyond it, and it's time to move along.
    auto const running_out = [](float at, float seen, float start, float size, float margin, float document) {
        return (at < start + margin && start > 0) || (at + seen > start + size - margin && start + size < document);
    };
    bool const move_it = running_out(origin.x, width, held.x, held.width, more_across / 4, document_width) || running_out(origin.y, height, held.y, held.height, more_down / 4, document_height);
    if (!move_it)
        return;
    // What's still wanted of what's there stays, and only what's new is drawn.
    auto const dx = static_cast<int>(Base::Math::round(static_cast<double>((centred.x - held.x) * scale)));
    auto const dy = static_cast<int>(Base::Math::round(static_cast<double>((centred.y - held.y) * scale)));
    auto& back = layer.backend->back_buffer();
    auto const& shown = layer.backend->shown_buffer();
    for (int y = 0; y < back.height(); ++y) {
        int const from_y = y + dy;
        if (from_y < 0 || from_y >= shown.height())
            continue;
        int const left = max(0, -dx);
        int const right = min(back.width(), back.width() - dx);
        if (right > left)
            __builtin_memcpy(back.scanline(y) + left, shown.scanline(from_y) + left + dx, static_cast<size_t>(right - left) * sizeof(Pixel));
    }
    layer.damage.clear();
    auto const old = held;
    layer.surface_rect = centred;
    if (centred.y < old.y)
        layer.damage.append({ centred.x, centred.y, centred.width, old.y - centred.y });
    if (centred.bottom() > old.bottom())
        layer.damage.append({ centred.x, old.bottom(), centred.width, centred.bottom() - old.bottom() });
    if (centred.x < old.x)
        layer.damage.append({ centred.x, centred.y, old.x - centred.x, centred.height });
    if (centred.right() > old.right())
        layer.damage.append({ old.right(), centred.y, centred.right() - old.right(), centred.height });
    layer.present_all = true;
}

void Window::sync_layer(View& view, size_t order)
{
    using WS::LayerProperty;
    auto& layer = *view.m_server_layer;
    auto const& backend = *layer.backend;
    if (view.m_scrolls_on_layer)
        fit_scrolling_surface(view);
    else
        fit_surface(view);

    // Where it goes, in the bounds of the layer it's in; and what's between them that cuts it off.
    View const* parent = layer.parent;
    auto position_in_parent = [&](View const& of) {
        FloatPoint point = of.m_frame.origin();
        if (&of != &view)
            point = point + of.m_translation;
        for (auto const* up = of.m_parent; up && up != parent; up = up->m_parent)
            point = point - up->m_bounds_origin + up->m_frame.origin() + up->m_translation;
        // A layer that scrolls is told how far itself.
        if (parent && !parent->m_scrolls_on_layer)
            point = point - parent->m_bounds_origin;
        return point;
    };
    auto const position = position_in_parent(view);
    FloatRect const frame { position.x, position.y, view.m_frame.width, view.m_frame.height };
    FloatRect clip { 0, 0, -1, -1 };
    for (auto const* up = view.m_parent; up && up != parent; up = up->m_parent) {
        if (!up->m_clips_children)
            continue;
        auto const at = position_in_parent(*up);
        FloatRect const cut { at.x, at.y, up->m_frame.width, up->m_frame.height };
        clip = clip.width < 0 ? cut : clip.intersected(cut);
    }

    if (frame != layer.sent_frame) {
        m_backend->layer_set(backend, LayerProperty::Frame, frame.x, frame.y, frame.width, frame.height);
        layer.sent_frame = frame;
    }
    if (clip != layer.sent_clip) {
        m_backend->layer_set(backend, LayerProperty::ClipRect, clip.x, clip.y, clip.width, clip.height);
        layer.sent_clip = clip;
    }
    if (view.m_translation != layer.sent_translation) {
        // (One that's been sent an animation is on its way there, and has been told already.)
        bool animating = false;
        for (auto const& queued : m_queued_layer_animations)
            animating = animating || (queued.view == &view && queued.property == LayerProperty::Translation);
        if (!animating)
            m_backend->layer_set(backend, LayerProperty::Translation, view.m_translation.x, view.m_translation.y);
        layer.sent_translation = view.m_translation;
    }
    if (view.m_opacity != layer.sent_opacity) {
        bool animating = false;
        for (auto const& queued : m_queued_layer_animations)
            animating = animating || (queued.view == &view && queued.property == LayerProperty::Opacity);
        if (!animating)
            m_backend->layer_set(backend, LayerProperty::Opacity, view.m_opacity);
        layer.sent_opacity = view.m_opacity;
    }
    FloatPoint bounds_origin {};
    if (view.m_scrolls_on_layer) {
        // On a whole pixel, for its text to stay as it was drawn.
        float const scale = static_cast<float>(m_backend->scale());
        bounds_origin = { static_cast<float>(Base::Math::round(static_cast<double>(view.m_bounds_origin.x * scale))) / scale, static_cast<float>(Base::Math::round(static_cast<double>(view.m_bounds_origin.y * scale))) / scale };
    }
    if (bounds_origin != layer.sent_bounds_origin) {
        m_backend->layer_set(backend, LayerProperty::BoundsOrigin, bounds_origin.x, bounds_origin.y);
        layer.sent_bounds_origin = bounds_origin;
    }
    auto const contents_origin = layer.surface_rect.origin();
    if (contents_origin != layer.sent_contents_origin) {
        m_backend->layer_set(backend, LayerProperty::ContentsOrigin, contents_origin.x, contents_origin.y);
        layer.sent_contents_origin = contents_origin;
    }
    bool const clips = view.m_scrolls_on_layer;
    if (clips != layer.sent_clips) {
        m_backend->layer_set(backend, LayerProperty::Clips, clips ? 1 : 0);
        layer.sent_clips = clips;
    }
    if (order != layer.sent_order) {
        m_backend->layer_set(backend, LayerProperty::Order, static_cast<float>(order));
        layer.sent_order = order;
    }
}

void Window::update_layers()
{
    if (!m_backend->supports_layers())
        return;
    if (m_layer_plan_stale)
        plan_layers();
    // Each in order, among those in the same place.
    for (size_t i = 0; i < m_layer_views.size(); ++i) {
        size_t order = 0;
        for (size_t before = 0; before < i; ++before) {
            if (m_layer_views[before]->m_server_layer->parent == m_layer_views[i]->m_server_layer->parent)
                ++order;
        }
        sync_layer(*m_layer_views[i], order);
    }
}

void Window::draw_layer(View& view)
{
    auto& layer = *view.m_server_layer;
    if (layer.damage.is_empty() && !layer.present_all)
        return;
    if (layer.surface_pixels.is_empty())
        return;
    auto& bitmap = layer.backend->back_buffer();
    float const scale = static_cast<float>(m_backend->scale());
    auto const& held = layer.surface_rect;
    IntRect total;
    for (auto const& area : layer.damage) {
        auto pixels = FloatRect { (area.x - held.x) * scale, (area.y - held.y) * scale, area.width * scale, area.height * scale }.enclosing_int_rect().intersected(bitmap.rect());
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
        painter.translate(-held.x, -held.y);
        Graphics graphics(painter, scale);
        FloatRect const dirty { held.x + static_cast<float>(pixels.x) / scale, held.y + static_cast<float>(pixels.y) / scale, static_cast<float>(pixels.width) / scale, static_cast<float>(pixels.height) / scale };
        view.draw_contents(graphics, dirty, view.m_scrolls_on_layer);
    }
    layer.damage.clear();
    if (layer.present_all) {
        total = bitmap.rect();
        layer.present_all = false;
    }
    if (!total.is_empty())
        m_backend->layer_present(*layer.backend, total);
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

void Window::set_sidebar_material(float width)
{
    m_sidebar_material_width = width;
    MaterialRegion region { { 0, 0, static_cast<int>(width), static_cast<int>(size().height) }, WS::Material::Sidebar };
    set_materials(Span<MaterialRegion const>(&region, 1));
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
    // Under the pointer, on the screen (the pointer's known in this window's points), wherever this window is.
    auto const origin = frame().origin();
    IntRect image_frame { origin.x + static_cast<int>(m_pointer.x - size.width / 2), origin.y + static_cast<int>(m_pointer.y - size.height / 2), static_cast<int>(size.width + 0.999f), static_cast<int>(size.height + 0.999f) };
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
        if (m_drop_target)
            m_drop_target->drag_moved(m_drop_target->from_window(position));
        break;
    case WS::DragPhase::Left:
        set_target(nullptr);
        break;
    case WS::DragPhase::Dropped:
        if (auto* target = target_at(position)) {
            target->drag_moved(target->from_window(position));
            target->perform_drop(type, data);
        }
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
    case WS::MouseKind::Magnify:
        return;
    case WS::MouseKind::Scroll:
    case WS::MouseKind::Wheel: {
        auto const step = kind == WS::MouseKind::Scroll ? 1.0f : Appearance::the().metric("scroll.pointsPerNotch"_sv);
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
    event.modifiers = modifiers & ~static_cast<u32>(WS::ModifierRepeat);
    event.repeat = modifiers & WS::ModifierRepeat;
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
    if (m_sidebar_material_width > 0)
        set_sidebar_material(m_sidebar_material_width);
    if (on_resize)
        on_resize(size);
}

void Window::handle_activation(bool active)
{
    if (active == m_key)
        return;
    m_key = active;
    if (active)
        WindowRegistry::became_key(*this);
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
    // Not on the screen, and nobody left to tell.
    m_layer_animation_endings.remove_all_matching([&](auto const& ending) { return ending.view == &view; });
    m_queued_layer_animations.remove_all_matching([&](auto const& queued) { return queued.view == &view; });
    for (size_t i = 0; i < m_layer_views.size(); ++i) {
        if (m_layer_views[i] == &view) {
            m_layer_views.remove(i);
            break;
        }
    }
    if (view.m_server_layer) {
        m_backend->layer_destroy(*view.m_server_layer->backend);
        view.m_server_layer = nullptr;
        layers_need_planning();
    } else if (view.m_wants_layer) {
        layers_need_planning();
    }
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
