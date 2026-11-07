/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Screenshots.h"
#include <Desktop.h>
#include <LibBase/String.h>
#include <LibImage/Image.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Application.h>
#include <LibUI/Appearance.h>
#include <LibUI/Event.h>
#include <LibUI/Graphics.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <LibUI/Platform.h>
#include <LibUI/View.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace DesktopShell {

using namespace UI;
using Gfx::FloatPoint;
using Gfx::FloatRect;
using Gfx::IntPoint;
using Gfx::IntRect;

namespace {

bool s_still;

Appearance const& tokens()
{
    return Appearance::the();
}

WindowServerProxy& server()
{
    return Application::the().connection().server();
}

// A small label on the screen, over anything: white, with a shadow.
void draw_label(Graphics& graphics, StringView text, FloatPoint at)
{
    auto const font = tokens().named_font("small"_sv);
    graphics.draw_text(text, font, { at.x + 1, at.y + 1 }, tokens().color("screenshot.labelShadow"_sv));
    graphics.draw_text(text, font, at, tokens().color("screenshot.label"_sv));
}

}

void set_screenshots_still(bool still)
{
    s_still = still;
}

// Over all of the screen, while a part of it, or a window, is being chosen.
class AreaView final : public View {
public:
    explicit AreaView(Screenshots& owner)
        : m_owner(owner)
    {
    }

    void start(bool window_mode, bool keep_selection)
    {
        m_window_mode = window_mode;
        m_dragging = false;
        m_kept = keep_selection ? m_owner.last_selection() : IntRect {};
        m_hovered_window = 0;
        m_hovered = {};
        set_needs_display();
    }

    [[nodiscard]] bool window_mode() const { return m_window_mode; }
    [[nodiscard]] IntRect kept() const { return m_kept; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        if (m_window_mode) {
            if (m_hovered_window)
                graphics.fill(m_hovered.to<float>(), t.color("screenshot.window"_sv));
            return;
        }
        IntRect shown = m_dragging ? selection() : m_kept;
        if (!shown.is_empty()) {
            auto const area = shown.to<float>();
            graphics.fill(area, t.color("screenshot.selection"_sv));
            graphics.stroke_rounded(area, 0, graphics.pixel(), t.color("screenshot.selectionBorder"_sv));
        }
        // By the pointer: where it is, or how big what's chosen is.
        StringBuilder text;
        if (m_dragging)
            text.appendff("{}  {}", shown.width, shown.height);
        else
            text.appendff("{}  {}", static_cast<int>(m_pointer.x), static_cast<int>(m_pointer.y));
        float const offset = t.metric("screenshot.labelOffset"_sv);
        draw_label(graphics, text.view(), { m_pointer.x + offset, m_pointer.y + offset + 10 });
    }

    [[nodiscard]] WS::CursorShape cursor() const override { return m_window_mode ? WS::CursorShape::PointingHand : WS::CursorShape::Crosshair; }

    bool mouse_down(MouseEvent const& event) override
    {
        m_pointer = event.position;
        m_on_toolbar = m_owner.toolbar_press(event.position.to<int>());
        if (m_on_toolbar || m_window_mode)
            return true;
        m_dragging = true;
        m_from = event.position;
        set_needs_display();
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        m_pointer = event.position;
        set_needs_display();
    }

    void mouse_moved(MouseEvent const& event) override
    {
        m_pointer = event.position;
        if (m_window_mode)
            look_under_pointer();
        set_needs_display();
    }

    void mouse_up(MouseEvent const& event) override
    {
        m_pointer = event.position;
        if (m_on_toolbar) {
            m_on_toolbar = false;
            m_owner.toolbar_release(event.position.to<int>());
            return;
        }
        if (m_window_mode) {
            look_under_pointer();
            if (m_hovered_window)
                m_owner.window_chosen(m_hovered_window);
            return;
        }
        if (!m_dragging)
            return;
        m_dragging = false;
        auto const chosen = selection();
        // A click on its own chooses nothing, as on a Mac.
        if (chosen.width < 2 || chosen.height < 2) {
            set_needs_display();
            return;
        }
        m_kept = chosen;
        m_owner.area_chosen(chosen);
    }

    bool key_down(KeyEvent const& event) override
    {
        if (event.code == KeyEscape) {
            m_owner.cancel_choosing();
            return true;
        }
        // Space: a window instead, and back again.
        if (event.code == KeySpace) {
            m_window_mode = !m_window_mode;
            m_dragging = false;
            if (m_window_mode)
                look_under_pointer();
            set_needs_display();
            if (window())
                window()->update_cursor();
            return true;
        }
        return true;
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_window_mode ? "Choose a window"_sv : "Choose an area"_sv)); }

private:
    [[nodiscard]] IntRect selection() const
    {
        int const left = static_cast<int>(min(m_from.x, m_pointer.x));
        int const top = static_cast<int>(min(m_from.y, m_pointer.y));
        int const right = static_cast<int>(max(m_from.x, m_pointer.x));
        int const bottom = static_cast<int>(max(m_from.y, m_pointer.y));
        return { left, top, right - left, bottom - top };
    }

    void look_under_pointer()
    {
        auto const under = server().window_under(static_cast<i32>(m_pointer.x), static_cast<i32>(m_pointer.y));
        if (under.is_error()) {
            m_hovered_window = 0;
            return;
        }
        m_hovered_window = under.value().window;
        m_hovered = { under.value().x, under.value().y, under.value().width, under.value().height };
    }

    Screenshots& m_owner;
    bool m_window_mode { false };
    bool m_dragging { false };
    bool m_on_toolbar { false };
    FloatPoint m_from;
    FloatPoint m_pointer;
    IntRect m_kept;
    u32 m_hovered_window { 0 };
    IntRect m_hovered;
};

// Screenshot's toolbar, at the bottom of the screen: what to take, its options, and taking it.
class ToolbarView final : public View {
public:
    enum class Part : u8 {
        Close,
        Screen,
        Window,
        Portion,
        RecordScreen,
        RecordPortion,
        Options,
        Capture,
        Count,
    };

    explicit ToolbarView(Screenshots& owner)
        : m_owner(owner)
    {
    }

    [[nodiscard]] static FloatRect rect_of(Part part)
    {
        auto const& t = tokens();
        float const padding = t.metric("screenshot.toolbarPadding"_sv);
        float const button = t.metric("screenshot.toolbarButton"_sv);
        float const gap = t.metric("screenshot.toolbarGap"_sv);
        float const divider = t.metric("screenshot.toolbarDivider"_sv);
        float const options = t.metric("screenshot.optionsWidth"_sv);
        float const capture = t.metric("screenshot.captureWidth"_sv);
        float const height = t.metric("screenshot.toolbarHeight"_sv);
        float x = padding;
        float const top = (height - button) / 2;
        auto next = [&](float width, float after) {
            FloatRect rect { x, top, width, button };
            x += width + after;
            return rect;
        };
        FloatRect rects[static_cast<int>(Part::Count)];
        rects[0] = next(button, divider);
        rects[1] = next(button, gap);
        rects[2] = next(button, gap);
        rects[3] = next(button, divider);
        rects[4] = next(button, gap);
        rects[5] = next(button, divider);
        rects[6] = next(options, gap);
        rects[7] = next(capture, padding);
        return rects[static_cast<int>(part)];
    }

    [[nodiscard]] static float width_wanted()
    {
        auto const capture = rect_of(Part::Capture);
        return capture.x + capture.width + tokens().metric("screenshot.toolbarPadding"_sv);
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        auto const symbol_color = t.color("screenshot.toolbarSymbol"_sv);
        auto const disabled = t.color("screenshot.toolbarDisabled"_sv);
        auto const mode = m_owner.options().mode;
        for (int i = 0; i < static_cast<int>(Part::Count); ++i) {
            auto const part = static_cast<Part>(i);
            auto const rect = rect_of(part);
            bool const chosen = (part == Part::Screen && mode == Screenshots::Mode::Screen) || (part == Part::Window && mode == Screenshots::Mode::Window) || (part == Part::Portion && mode == Screenshots::Mode::Portion);
            if (chosen)
                graphics.fill_rounded(rect, t.metric("screenshot.toolbarButtonRadius"_sv), t.color("screenshot.toolbarChosen"_sv));
            else if (i == m_pressed)
                graphics.fill_rounded(rect, t.metric("screenshot.toolbarButtonRadius"_sv), t.color("screenshot.toolbarPressed"_sv));
            draw_part(graphics, part, rect, (part == Part::RecordScreen || part == Part::RecordPortion) ? disabled : symbol_color);
        }
        // Lines between the groups.
        auto const line = t.color("screenshot.toolbarLine"_sv);
        Part const groups_end[] = { Part::Close, Part::Portion, Part::RecordPortion };
        for (auto after : groups_end) {
            auto const rect = rect_of(after);
            float const x = graphics.snap(rect.x + rect.width + t.metric("screenshot.toolbarDivider"_sv) / 2);
            graphics.fill({ x, rect.y + 6, graphics.pixel(), rect.height - 12 }, line);
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        press_at(event.position);
        return true;
    }

    void mouse_up(MouseEvent const& event) override
    {
        release_at(event.position);
    }

    // (From the chooser too, which has the pointer while the toolbar's up, as the toolbar's over it.)
    void press_at(FloatPoint point)
    {
        m_pressed = part_at(point);
        set_needs_display();
    }

    void release_at(FloatPoint point)
    {
        int const released = part_at(point);
        int const pressed = m_pressed;
        m_pressed = -1;
        set_needs_display();
        if (released < 0 || released != pressed)
            return;
        switch (static_cast<Part>(released)) {
        case Part::Close:
            m_owner.toolbar_close();
            return;
        case Part::Screen:
            m_owner.toolbar_mode(Screenshots::Mode::Screen);
            return;
        case Part::Window:
            m_owner.toolbar_mode(Screenshots::Mode::Window);
            return;
        case Part::Portion:
            m_owner.toolbar_mode(Screenshots::Mode::Portion);
            return;
        case Part::Options: {
            auto const rect = rect_of(Part::Options);
            auto const origin = window()->frame().origin();
            m_owner.open_options({ origin.x + static_cast<int>(rect.x), origin.y + static_cast<int>(rect.y) });
            return;
        }
        case Part::Capture:
            m_owner.toolbar_capture();
            return;
        default:
            return;
        }
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Toolbar; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Screenshot"_sv)); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        auto const mode = m_owner.options().mode;
        auto add = [&](Part part, StringView label, bool chosen, bool enabled) {
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, label, rect_of(part), chosen ? "chosen"_sv : StringView {}, enabled));
        };
        add(Part::Close, "Close"_sv, false, true);
        add(Part::Screen, "Capture Entire Screen"_sv, mode == Screenshots::Mode::Screen, true);
        add(Part::Window, "Capture Selected Window"_sv, mode == Screenshots::Mode::Window, true);
        add(Part::Portion, "Capture Selected Portion"_sv, mode == Screenshots::Mode::Portion, true);
        add(Part::RecordScreen, "Record Entire Screen"_sv, false, false);
        add(Part::RecordPortion, "Record Selected Portion"_sv, false, false);
        add(Part::Options, "Options"_sv, false, true);
        add(Part::Capture, "Capture"_sv, false, true);
    }

private:
    [[nodiscard]] int part_at(FloatPoint point) const
    {
        for (int i = 0; i < static_cast<int>(Part::Count); ++i) {
            if (rect_of(static_cast<Part>(i)).contains(point))
                return i;
        }
        return -1;
    }

    // A dashed line, as of what's to be chosen.
    static void dashed(Graphics& graphics, FloatRect rect, Gfx::Color color)
    {
        float const dash = 2;
        for (float x = rect.x; x < rect.x + rect.width; x += dash * 2) {
            graphics.fill({ x, rect.y, min(dash, rect.x + rect.width - x), 1.25f }, color);
            graphics.fill({ x, rect.y + rect.height - 1.25f, min(dash, rect.x + rect.width - x), 1.25f }, color);
        }
        for (float y = rect.y; y < rect.y + rect.height; y += dash * 2) {
            graphics.fill({ rect.x, y, 1.25f, min(dash, rect.y + rect.height - y) }, color);
            graphics.fill({ rect.x + rect.width - 1.25f, y, 1.25f, min(dash, rect.y + rect.height - y) }, color);
        }
    }

    static void draw_part(Graphics& graphics, Part part, FloatRect rect, Gfx::Color color)
    {
        auto const& t = tokens();
        float const width = t.metric("screenshot.symbolWidth"_sv);
        float const height = t.metric("screenshot.symbolHeight"_sv);
        FloatRect const symbol { graphics.snap(rect.x + (rect.width - width) / 2), graphics.snap(rect.y + (rect.height - height) / 2), width, height };
        float const line = 1.5f;
        switch (part) {
        case Part::Close: {
            float const size = 10;
            FloatPoint const middle { rect.x + rect.width / 2, rect.y + rect.height / 2 };
            graphics.line({ middle.x - size / 2, middle.y - size / 2 }, { middle.x + size / 2, middle.y + size / 2 }, line, color, true);
            graphics.line({ middle.x + size / 2, middle.y - size / 2 }, { middle.x - size / 2, middle.y + size / 2 }, line, color, true);
            return;
        }
        case Part::Screen:
            graphics.stroke_rounded(symbol, 2, line, color);
            graphics.fill_rounded(symbol.inflated(-3), 1, color);
            return;
        case Part::Window:
            graphics.stroke_rounded(symbol, 2, line, color);
            graphics.fill_rounded({ symbol.x + 4, symbol.y + 4, symbol.width - 8, symbol.height - 8 }, 1.5f, color.with_opacity(0.55f));
            graphics.fill({ symbol.x + 4, symbol.y + 4, symbol.width - 8, 2 }, color);
            return;
        case Part::Portion:
            graphics.stroke_rounded(symbol, 2, line, color);
            dashed(graphics, symbol.inflated(-3.5f), color);
            return;
        case Part::RecordScreen:
            graphics.stroke_rounded(symbol, 2, line, color);
            graphics.fill_circle({ symbol.x + symbol.width - 4, symbol.y + symbol.height - 4 }, 4, color);
            return;
        case Part::RecordPortion:
            graphics.stroke_rounded(symbol, 2, line, color);
            dashed(graphics, symbol.inflated(-3.5f), color);
            graphics.fill_circle({ symbol.x + symbol.width - 4, symbol.y + symbol.height - 4 }, 4, color);
            return;
        case Part::Options: {
            auto const font = t.named_font("body"_sv);
            graphics.draw_text_centered("Options \xE2\x8C\x84"_sv, font, rect, color);
            return;
        }
        case Part::Capture: {
            graphics.fill_rounded(rect.inflated(-4), t.metric("screenshot.toolbarButtonRadius"_sv), t.color("screenshot.captureFill"_sv));
            graphics.draw_text_centered("Capture"_sv, t.named_font("body"_sv), rect, t.color("screenshot.captureText"_sv));
            return;
        }
        default:
            return;
        }
    }

    Screenshots& m_owner;
    int m_pressed { -1 };
};

// The one just taken, small, in the corner: pressed, it's opened; pushed aside, it's saved at once.
class ThumbnailView final : public View {
public:
    explicit ThumbnailView(Screenshots& owner)
        : m_owner(owner)
    {
    }

    void set_picture(RefPtr<Gfx::Bitmap> picture)
    {
        m_picture = move(picture);
        // A window's, with its shadow round it on nothing, has no edge of its own to draw.
        m_framed = m_picture && Gfx::pixel_alpha(m_picture->scanline(0)[0]) == 255;
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        if (!m_picture)
            return;
        FloatRect const all { 0, 0, width(), height() };
        graphics.painter().draw_bitmap(*m_picture, all);
        if (m_framed)
            graphics.stroke_rounded(all, tokens().metric("screenshot.thumbnailRadius"_sv), graphics.pixel(), tokens().color("screenshot.thumbnailBorder"_sv));
    }

    bool mouse_down(MouseEvent const& event) override
    {
        m_pressed_at = event.window_position;
        m_moved = false;
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (event.window_position.x - m_pressed_at.x > 4)
            m_moved = true;
    }

    void mouse_up(MouseEvent const& event) override
    {
        // Pushed towards the edge it came from: away, and saved now.
        if (m_moved && event.window_position.x - m_pressed_at.x > tokens().metric("screenshot.swipeDistance"_sv)) {
            m_owner.thumbnail_swiped();
            return;
        }
        if (!m_moved)
            m_owner.thumbnail_clicked();
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Image; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Screenshot"_sv)); }

private:
    Screenshots& m_owner;
    RefPtr<Gfx::Bitmap> m_picture;
    bool m_framed { true };
    FloatPoint m_pressed_at;
    bool m_moved { false };
};

Screenshots::Screenshots() = default;
Screenshots::~Screenshots() = default;

void Screenshots::register_keys()
{
    u32 const modifiers = WS::ModifierCommand | WS::ModifierShift;
    (void)server().register_hotkey(Key3, modifiers);
    (void)server().register_hotkey(Key4, modifiers);
    (void)server().register_hotkey(Key5, modifiers);
}

bool Screenshots::hotkey(u16 code, u32)
{
    switch (code) {
    case Key3:
        start_after_timer([this] { take_screen(); });
        return true;
    case Key4:
        choose_area(false);
        return true;
    case Key5:
        show_toolbar();
        return true;
    default:
        return false;
    }
}

void Screenshots::start_after_timer(Function<void()> action)
{
    // (The timer is the toolbar's to set, and only what's started from it waits.)
    if (!m_from_toolbar || m_options.timer_seconds == 0) {
        action();
        return;
    }
    auto shared = adopt_own(*new Function<void()>(move(action)));
    auto* raw = shared.leak_ptr();
    (void)Application::the().add_timer(static_cast<u64>(m_options.timer_seconds) * 1000, false, [raw] {
        (*raw)();
        delete raw;
    });
}

void Screenshots::take_screen()
{
    auto png = Application::the().connection().capture_screen();
    if (png.is_error())
        return;
    deliver(png.release_value());
}

void Screenshots::choose_area(bool window_at_first)
{
    if (!m_area_window) {
        auto const screen = Platform::the().screen_size();
        m_area_window = adopt_own(*new UI::Window("Screenshot Selection"_sv, { 0, 0, screen.width, screen.height }, WS::WindowBare | WS::WindowPopup));
        m_area_window->set_transparent(true);
        m_area_window->backend().set_level(WS::LevelPopup);
        m_area = &m_area_window->root_view().add<AreaView>(*this);
        m_area->set_frame({ 0, 0, static_cast<float>(screen.width), static_cast<float>(screen.height) });
        m_area_window->set_pointer_catcher(m_area);
        m_area_window->set_key_interceptor(m_area);
    }
    m_area->start(window_at_first, m_from_toolbar && m_options.remember_selection);
    m_area_window->show();
    m_area_window->backend().grab_pointer(true);
    // The toolbar stays over it.
    if (m_toolbar_window && m_from_toolbar)
        m_toolbar_window->show();
}

void Screenshots::hide_area()
{
    if (!m_area_window)
        return;
    m_area_window->backend().grab_pointer(false);
    m_area_window->hide();
}

void Screenshots::cancel_choosing()
{
    hide_area();
    if (m_from_toolbar)
        toolbar_close();
}

void Screenshots::area_chosen(IntRect points)
{
    m_last_selection = points;
    // From the toolbar, a part is chosen, and kept, until Capture.
    if (m_from_toolbar)
        return;
    hide_area();
    // Taken once what chose it has gone from the screen.
    (void)Application::the().add_timer(60, false, [this, points] {
        auto png = Application::the().connection().capture_screen(points);
        if (!png.is_error())
            deliver(png.release_value());
    });
}

void Screenshots::window_chosen(u32 window)
{
    hide_area();
    if (m_from_toolbar) {
        m_toolbar_window->hide();
        m_from_toolbar = false;
    }
    (void)Application::the().add_timer(60, false, [this, window] {
        auto answer = server().capture_window(window);
        if (answer.is_error() || answer.value().error)
            return;
        deliver(move(answer.value().png));
    });
}

void Screenshots::show_toolbar()
{
    auto const& t = tokens();
    auto const screen = Platform::the().screen_size();
    int const width = static_cast<int>(ToolbarView::width_wanted());
    int const height = static_cast<int>(t.metric("screenshot.toolbarHeight"_sv));
    IntRect const frame { (screen.width - width) / 2, screen.height - height - static_cast<int>(t.metric("screenshot.toolbarBottom"_sv)), width, height };
    if (!m_toolbar_window) {
        m_toolbar_window = adopt_own(*new UI::Window("Screenshot"_sv, frame, WS::WindowBare | WS::WindowPopup));
        m_toolbar_window->set_transparent(true);
        m_toolbar_window->backend().set_level(WS::LevelPopup);
        m_toolbar = &m_toolbar_window->root_view().add<ToolbarView>(*this);
        MaterialRegion const material { { 0, 0, width, height }, WS::Material::HUD, static_cast<int>(t.metric("screenshot.toolbarRadius"_sv)) };
        m_toolbar_window->set_materials(Span<MaterialRegion const>(&material, 1));
    }
    m_toolbar->set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
    m_toolbar_window->set_frame(frame);
    m_from_toolbar = true;
    toolbar_mode(m_options.mode);
    m_toolbar_window->show();
}

void Screenshots::toolbar_mode(Mode mode)
{
    m_options.mode = mode;
    if (m_toolbar)
        m_toolbar->set_needs_display();
    switch (mode) {
    case Mode::Screen:
        hide_area();
        break;
    case Mode::Window:
    case Mode::Portion:
        choose_area(mode == Mode::Window);
        break;
    }
}

void Screenshots::toolbar_capture()
{
    auto const mode = m_options.mode;
    auto const area = m_last_selection;
    toolbar_close();
    m_from_toolbar = true;
    start_after_timer([this, mode, area] {
        m_from_toolbar = false;
        if (mode == Mode::Screen) {
            take_screen();
        } else if (mode == Mode::Portion && !area.is_empty()) {
            auto png = Application::the().connection().capture_screen(area);
            if (!png.is_error())
                deliver(png.release_value());
        }
    });
    if (m_options.timer_seconds == 0)
        m_from_toolbar = false;
}

bool Screenshots::toolbar_press(IntPoint screen)
{
    if (!m_from_toolbar || !m_toolbar_window || !m_toolbar_window->is_shown())
        return false;
    auto const frame = m_toolbar_window->frame();
    if (!frame.contains(screen))
        return false;
    m_toolbar->press_at({ static_cast<float>(screen.x - frame.x), static_cast<float>(screen.y - frame.y) });
    return true;
}

void Screenshots::toolbar_release(IntPoint screen)
{
    if (!m_toolbar_window)
        return;
    auto const frame = m_toolbar_window->frame();
    m_toolbar->release_at({ static_cast<float>(screen.x - frame.x), static_cast<float>(screen.y - frame.y) });
}

void Screenshots::toolbar_close()
{
    hide_area();
    if (m_toolbar_window)
        m_toolbar_window->hide();
    if (m_options_menu)
        m_options_menu->close();
    m_from_toolbar = false;
}

void Screenshots::open_options(IntPoint screen_point)
{
    m_options_menu = adopt_own(*new Menu);
    auto& menu = *m_options_menu;
    (void)menu.add_heading("Save to"_sv);
    menu.add("Desktop"_sv, [this] { m_options.save_to_documents = false; }).checked = !m_options.save_to_documents;
    menu.add("Documents"_sv, [this] { m_options.save_to_documents = true; }).checked = m_options.save_to_documents;
    menu.add_separator();
    (void)menu.add_heading("Timer"_sv);
    menu.add("None"_sv, [this] { m_options.timer_seconds = 0; }).checked = m_options.timer_seconds == 0;
    menu.add("5 Seconds"_sv, [this] { m_options.timer_seconds = 5; }).checked = m_options.timer_seconds == 5;
    menu.add("10 Seconds"_sv, [this] { m_options.timer_seconds = 10; }).checked = m_options.timer_seconds == 10;
    menu.add_separator();
    (void)menu.add_heading("Options"_sv);
    menu.add("Show Floating Thumbnail"_sv, [this] { m_options.floating_thumbnail = !m_options.floating_thumbnail; }).checked = m_options.floating_thumbnail;
    menu.add("Remember Last Selection"_sv, [this] { m_options.remember_selection = !m_options.remember_selection; }).checked = m_options.remember_selection;
    // Above the button, as the toolbar's at the bottom of the screen.
    auto const size = menu.size();
    menu.open(*m_toolbar_window, { screen_point.x, screen_point.y - static_cast<int>(size.height) - 6 });
}

String Screenshots::file_path() const
{
    // "Screenshot 2026-01-15 at 9.41.00 AM.png", with a narrow space before the AM, in the folder chosen.
    struct tm parts {};
    if (s_still) {
        parts.tm_year = 2026 - 1900;
        parts.tm_mon = 0;
        parts.tm_mday = 15;
        parts.tm_hour = 9;
        parts.tm_min = 41;
        parts.tm_sec = 0;
    } else {
        time_t const now = time(nullptr);
        localtime_r(&now, &parts);
    }
    int const hour = parts.tm_hour % 12 == 0 ? 12 : parts.tm_hour % 12;
    StringBuilder name;
    name.appendff("Screenshot {:04}-{:02}-{:02} at {}.{:02}.{:02}\xE2\x80\xAF{}", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, hour, parts.tm_min, parts.tm_sec, parts.tm_hour < 12 ? "AM"_sv : "PM"_sv);
    StringBuilder folder;
    folder.append(Desktop::home_directory().view());
    folder.append(m_options.save_to_documents ? "/Documents/"_sv : "/Desktop/"_sv);
    // Another in the same second: " (2)", and so on.
    for (int copy = 1;; ++copy) {
        StringBuilder path;
        path.append(folder.view());
        path.append(name.view());
        if (copy > 1)
            path.appendff(" ({})", copy);
        path.append(".png"_sv);
        auto candidate = MUST(path.to_string());
        struct stat information {};
        if (stat(candidate.characters(), &information) != 0 && (m_pending_name.is_empty() || candidate.view() != m_pending_name.view()))
            return candidate;
    }
}

void Screenshots::deliver(Vector<u8> png)
{
    Platform::the().play_sound("screenshot"_sv);
    // One still waiting goes first.
    save_now();
    m_pending_name = file_path();
    m_pending = move(png);
    if (!m_options.floating_thumbnail) {
        save_now();
        return;
    }
    auto picture = Image::decode_png(m_pending.span());
    if (picture.is_error()) {
        save_now();
        return;
    }
    auto const& t = tokens();
    auto const screen = Platform::the().screen_size();
    int const scale = Application::the().connection().scale();
    float const width_points = static_cast<float>(picture.value()->width()) / static_cast<float>(scale);
    float const height_points = static_cast<float>(picture.value()->height()) / static_cast<float>(scale);
    float const longest = t.metric("screenshot.thumbnailSize"_sv);
    float const factor = longest / max(width_points, height_points);
    int const width = max(1, static_cast<int>(width_points * factor));
    int const height = max(1, static_cast<int>(height_points * factor));
    int const margin = static_cast<int>(t.metric("screenshot.thumbnailMargin"_sv));
    IntRect const frame { screen.width - width - margin, screen.height - height - margin, width, height };
    // A window's comes with its own shadow, so its thumbnail has no other; the rest have a thumbnail's.
    bool const bare = Gfx::pixel_alpha(picture.value()->scanline(0)[0]) != 255;
    if (m_thumbnail_window && bare != m_thumbnail_bare) {
        m_thumbnail_window->hide();
        auto* old = m_thumbnail_window.leak_ptr();
        Platform::the().defer([old] { delete old; });
        m_thumbnail = nullptr;
    }
    m_thumbnail_bare = bare;
    if (!m_thumbnail_window) {
        m_thumbnail_window = adopt_own(*new UI::Window("Screenshot Thumbnail"_sv, frame, bare ? (WS::WindowBare | WS::WindowPopup) : WS::WindowPopup));
        m_thumbnail_window->set_transparent(true);
        m_thumbnail_window->backend().set_level(WS::LevelPopup);
        m_thumbnail_window->backend().set_corner_radius(static_cast<int>(t.metric("screenshot.thumbnailRadius"_sv)));
        m_thumbnail = &m_thumbnail_window->root_view().add<ThumbnailView>(*this);
    }
    m_thumbnail->set_picture(picture.release_value());
    m_thumbnail->set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
    m_thumbnail_window->set_frame(frame);
    m_thumbnail_window->show();
    if (m_thumbnail_timer >= 0)
        Application::the().cancel_timer(m_thumbnail_timer);
    m_thumbnail_timer = Application::the().add_timer(static_cast<u64>(t.metric("screenshot.thumbnailSeconds"_sv) * 1000), false, [this] {
        m_thumbnail_timer = -1;
        save_now();
    });
}

void Screenshots::save_now()
{
    if (m_thumbnail_timer >= 0) {
        Application::the().cancel_timer(m_thumbnail_timer);
        m_thumbnail_timer = -1;
    }
    if (m_thumbnail_window)
        m_thumbnail_window->hide();
    if (m_pending_name.is_empty())
        return;
    int const fd = open(m_pending_name.characters(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd >= 0) {
        size_t written = 0;
        while (written < m_pending.size()) {
            ssize_t const now = write(fd, m_pending.data() + written, m_pending.size() - written);
            if (now <= 0)
                break;
            written += static_cast<size_t>(now);
        }
        close(fd);
        printf("screenshot: saved %s\n", m_pending_name.characters());
        fflush(stdout);
    }
    m_pending.clear();
    m_pending_name = {};
}

void Screenshots::thumbnail_clicked()
{
    auto const path = MUST(String::from_view(m_pending_name.view()));
    save_now();
    if (!path.is_empty())
        (void)Desktop::open_path(path.view());
}

void Screenshots::thumbnail_swiped()
{
    save_now();
}

}
