/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/ScrollView.h>
#include <signal.h>
#include <stdio.h>

// What is on this machine, and (once there is a network to fetch them over) what else
// could be. Nothing is listed that does not exist.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

class ApplicationRow final : public View {
public:
    explicit ApplicationRow(Desktop::AppInfo info)
        : m_info(move(info))
    {
        m_open = &add<Button>("Open"_sv);
        m_open->on_action = [this] {
            auto launched = Desktop::launch(m_info);
            if (launched.is_error())
                fprintf(stderr, "Software: cannot open %s\n", m_info.name.characters());
        };
    }

    [[nodiscard]] FloatSize intrinsic_size() const override { return { -1, tokens().metric("software.rowHeight"_sv) }; }

    void layout() override
    {
        auto size = m_open->intrinsic_size();
        float padding = tokens().metric("software.padding"_sv);
        m_open->set_frame({ width() - padding - size.width, (height() - size.height) / 2, size.width, size.height });
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float padding = t.metric("software.padding"_sv);
        float icon_size = t.metric("software.iconSize"_sv);
        float y = (height() - icon_size) / 2;
        auto icon = Desktop::icon_named(m_info.icon.view(), static_cast<int>(icon_size), static_cast<int>(graphics.scale()));
        if (icon)
            graphics.painter().draw_bitmap(*icon, FloatRect { padding, y, icon_size, icon_size });
        float x = padding + icon_size + 12;
        float room = width() - x - padding - m_open->width() - 8;
        graphics.draw_text_truncated(m_info.name.view(), t.named_font("headline"_sv), { x, height() / 2 - 3 }, room, t.color("label"_sv));
        graphics.draw_text_truncated(m_info.kind.is_empty() ? m_info.identifier.view() : m_info.kind.view(), t.named_font("subheadline"_sv), { x, height() / 2 + 14 }, room, t.color("secondaryLabel"_sv));
        graphics.fill(graphics.snapped({ x, height() - graphics.pixel(), width() - x, graphics.pixel() }), t.color("separator"_sv));
    }

private:
    Desktop::AppInfo m_info;
    Button* m_open { nullptr };
};

class StoreContent final : public View {
public:
    void draw(Graphics& graphics) override
    {
        auto sidebar = tokens().metric("software.sidebarWidth"_sv);
        graphics.fill({ sidebar, 0, width() - sidebar, height() }, tokens().color("windowBackground"_sv));
    }

    void layout() override
    {
        for (auto const& child : children())
            child->set_frame({ 0, 0, width(), height() });
    }
};

class Software {
public:
    Software()
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("software.width"_sv));
        int height = static_cast<int>(t.metric("software.height"_sv));
        auto& window = Application::the().make_window<UI::Window>("Software"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle);
        window.center_on_screen();
        window.set_transparent(true);
        window.set_sidebar_material(t.metric("software.sidebarWidth"_sv));
        window.set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& root = window.set_content<StoreContent>();
        auto& split = root.add<SplitView>();
        float sidebar_width = t.metric("software.sidebarWidth"_sv);
        auto& sidebar = split.add_pane<Sidebar>(sidebar_width, sidebar_width);
        sidebar.set_top_padding(t.metric("window.toolbarCompact"_sv));
        sidebar.add_section("Software"_sv);
        (void)sidebar.add_row("Installed"_sv, Symbol::Applications);
        (void)sidebar.add_row("Available"_sv, Symbol::Download);
        sidebar.on_select = [this](size_t row) { show(row); };

        auto& right = split.add_pane<StackView>(0, 300, Axis::Vertical);
        auto& toolbar = right.add<Toolbar>();
        toolbar.set_left_padding(t.metric("toolbar.paddingRight"_sv));
        m_title = &toolbar.add_item<Label>("Installed"_sv, t.named_font("headline"_sv));
        toolbar.add_flexible_space();

        m_installed = &right.add<ScrollView>();
        m_installed->set_stretch(1, 1);
        m_installed->set_scrolls_horizontally(false);
        auto& rows = m_installed->make_document<StackView>(Axis::Vertical);
        auto applications = Desktop::installed_applications();
        for (size_t i = 0; i < applications.size(); ++i)
            (void)rows.add<ApplicationRow>(move(applications[i]));

        m_available = &right.add<StackView>(Axis::Vertical, 8, Alignment::Center, Insets::all(40));
        m_available->set_stretch(1, 1);
        (void)m_available->add<Label>("Nothing to get yet"_sv, t.named_font("headline"_sv));
        auto& explanation = m_available->add<Label>("Software comes from a signed catalog over the network, which this version cannot reach."_sv);
        explanation.set_style(LabelStyle::Secondary);
        m_available->set_hidden(true);

        window.show();
        sidebar.select_row(0);
    }

private:
    void show(size_t row)
    {
        m_title->set_text(row == 0 ? "Installed"_sv : "Available"_sv);
        m_installed->set_hidden(row != 0);
        m_available->set_hidden(row == 0);
    }

    Label* m_title { nullptr };
    ScrollView* m_installed { nullptr };
    StackView* m_available { nullptr };
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Software"_sv, "com.vulture.Software"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Software: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());
    Software center;
    printf("software: ready\n");
    fflush(stdout);
    return application->exec();
}
