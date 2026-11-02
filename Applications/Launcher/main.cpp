/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibUI/Application.h>
#include <LibUI/Motion.h>
#include <LibUI/TextField.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

// Every application, in a grid over the whole screen, in pages. A click or Return opens
// one and it goes away, as it does on Escape and on a click anywhere else.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

class Grid final : public View {
public:
    Grid()
    {
        m_all = Desktop::installed_applications();
        filter({});
    }

    Function<void()> on_finished;

    void filter(StringView text)
    {
        m_shown.clear();
        for (size_t i = 0; i < m_all.size(); ++i) {
            auto const& application = m_all[i];
            if (application.identifier.view() == "com.vulture.launchpad"_sv)
                continue;
            if (text.is_empty() || contains_ignoring_case(application.name.view(), text))
                m_shown.append(i);
        }
        m_page = 0;
        m_selected = m_shown.is_empty() ? -1 : 0;
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float icon_size = t.metric("launchpad.iconSize"_sv);
        float cell_width = t.metric("launchpad.cellWidth"_sv);
        float cell_height = t.metric("launchpad.cellHeight"_sv);
        auto font = t.named_font("body"_sv);
        size_t per_page = rows() * columns();
        size_t first = static_cast<size_t>(m_page) * per_page;
        int scale = static_cast<int>(graphics.scale());
        for (size_t i = first; i < m_shown.size() && i < first + per_page; ++i) {
            auto const& application = m_all[m_shown[i]];
            auto rect = cell_rect(i - first);
            auto icon = Desktop::icon_named(application.icon.view(), static_cast<int>(icon_size), scale);
            float x = rect.x + (cell_width - icon_size) / 2;
            if (static_cast<int>(i) == m_selected)
                graphics.fill_rounded({ x - 8, rect.y - 8, icon_size + 16, icon_size + 16 }, 18, t.color("launchpad.fill"_sv));
            if (icon)
                graphics.painter().draw_bitmap(*icon, FloatRect { x, rect.y, icon_size, icon_size });
            float text_width = min(Appearance::text_width(font, application.name.view()), cell_width - 8);
            graphics.draw_text_truncated(application.name.view(), font, { rect.x + (cell_width - text_width) / 2, rect.y + icon_size + 20 }, cell_width - 8, t.color("launchpad.text"_sv));
            (void)cell_height;
        }
        // A dot for each page.
        int pages = page_count();
        if (pages > 1) {
            float spacing = 18;
            float x = (width() - spacing * static_cast<float>(pages - 1)) / 2;
            for (int i = 0; i < pages; ++i)
                graphics.fill_circle({ x + static_cast<float>(i) * spacing, height() - 20 }, 4, t.color("launchpad.text"_sv).with_opacity(i == m_page ? 1 : 0.35f));
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        size_t per_page = rows() * columns();
        size_t first = static_cast<size_t>(m_page) * per_page;
        for (size_t i = first; i < m_shown.size() && i < first + per_page; ++i) {
            if (cell_rect(i - first).contains(event.position)) {
                open(i);
                return true;
            }
        }
        if (on_finished)
            on_finished();
        return true;
    }

    bool scroll_wheel(ScrollEvent const& event) override
    {
        if (event.delta_y > 0 && m_page + 1 < page_count())
            ++m_page;
        else if (event.delta_y < 0 && m_page > 0)
            --m_page;
        set_needs_display();
        return true;
    }

    bool handle_key(KeyEvent const& event)
    {
        int across = static_cast<int>(columns());
        int count = static_cast<int>(m_shown.size());
        if (count == 0)
            return false;
        int next = m_selected;
        switch (event.code) {
        case KeyLeft:
            next -= 1;
            break;
        case KeyRight:
            next += 1;
            break;
        case KeyUp:
            next -= across;
            break;
        case KeyDown:
            next += across;
            break;
        case KeyEnter:
            if (m_selected >= 0)
                open(static_cast<size_t>(m_selected));
            return true;
        default:
            return false;
        }
        m_selected = clamp(next, 0, count - 1);
        m_page = m_selected / static_cast<int>(rows() * columns());
        set_needs_display();
        return true;
    }

private:
    [[nodiscard]] static bool contains_ignoring_case(StringView text, StringView part)
    {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        for (size_t i = 0; i + part.length() <= text.length(); ++i) {
            bool same = true;
            for (size_t j = 0; j < part.length(); ++j) {
                if (lower(text[i + j]) != lower(part[j]))
                    same = false;
            }
            if (same)
                return true;
        }
        return false;
    }

    [[nodiscard]] size_t columns() const { return static_cast<size_t>(tokens().metric("launchpad.columns"_sv)); }
    [[nodiscard]] size_t rows() const { return max(static_cast<size_t>((height() - 40) / tokens().metric("launchpad.cellHeight"_sv)), static_cast<size_t>(1)); }
    [[nodiscard]] int page_count() const
    {
        size_t per_page = rows() * columns();
        return static_cast<int>((m_shown.size() + per_page - 1) / per_page);
    }

    [[nodiscard]] FloatRect cell_rect(size_t index) const
    {
        auto const& t = tokens();
        float cell_width = t.metric("launchpad.cellWidth"_sv);
        float cell_height = t.metric("launchpad.cellHeight"_sv);
        size_t across = columns();
        float total = static_cast<float>(across) * cell_width;
        float left = (width() - total) / 2;
        return { left + static_cast<float>(index % across) * cell_width, static_cast<float>(index / across) * cell_height, cell_width, cell_height };
    }

    void open(size_t index)
    {
        auto const& application = m_all[m_shown[index]];
        (void)Desktop::launch(application);
        if (on_finished)
            on_finished();
    }

    Vector<Desktop::AppInfo> m_all;
    Vector<size_t> m_shown;
    int m_page { 0 };
    int m_selected { -1 };
};

class SearchBox final : public TextField {
public:
    SearchBox()
        : TextField({}, FieldStyle::Rounded)
    {
        set_placeholder("Search"_sv);
    }

    Function<bool(KeyEvent const&)> on_key;

    bool key_down(KeyEvent const& event) override
    {
        if (on_key && on_key(event))
            return true;
        return TextField::key_down(event);
    }
};

class Screen final : public View {
public:
    Screen()
    {
        auto const& t = tokens();
        m_search = &add<SearchBox>();
        m_grid = &add<Grid>();
        m_search->on_change = [this] { m_grid->filter(m_search->text()); };
        m_search->on_key = [this](KeyEvent const& event) {
            if (event.code == KeyEscape) {
                if (m_grid->on_finished)
                    m_grid->on_finished();
                return true;
            }
            return m_grid->handle_key(event);
        };
        (void)t;
    }

    [[nodiscard]] SearchBox& search() { return *m_search; }
    [[nodiscard]] Grid& grid() { return *m_grid; }

    void layout() override
    {
        auto const& t = tokens();
        float search_width = t.metric("launchpad.searchWidth"_sv);
        float top = t.metric("launchpad.topPadding"_sv);
        m_search->set_frame({ (width() - search_width) / 2, top, search_width, 22 });
        m_grid->set_frame({ 0, top + 60, width(), height() - top - 60 });
    }

    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, tokens().color("launchpad.fill"_sv));
    }

private:
    SearchBox* m_search { nullptr };
    Grid* m_grid { nullptr };
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    bool still = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            still = true;
    }
    auto application_or_error = Application::create();
    if (application_or_error.is_error())
        return 1;
    auto application = application_or_error.release_value();
    if (still)
        set_motion_enabled(false);
    auto screen = Platform::the().screen_size();
    auto window = adopt_own(*new UI::Window("Launchpad"_sv, { 0, 0, screen.width, screen.height }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
    window->set_transparent(true);
    window->backend().set_level(WS::LevelPopup);
    MaterialRegion material { { 0, 0, screen.width, screen.height }, WS::Material::UnderWindow };
    window->set_materials(Span<MaterialRegion const>(&material, 1));
    auto& view = window->set_content<Screen>();
    auto* app = application.ptr();
    view.grid().on_finished = [app] { app->quit(); };
    window->on_key_change = [app](bool active) {
        if (!active)
            app->quit();
    };
    window->show();
    window->update_layout_now();
    window->make_first_responder(&view.search());
    printf("launchpad: shown\n");
    fflush(stdout);
    return application->exec();
}
