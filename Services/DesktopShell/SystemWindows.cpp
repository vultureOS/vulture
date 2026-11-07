/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SystemWindows.h"
#include <LibBase/String.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Application.h>
#include <LibUI/Appearance.h>
#include <LibUI/Controls.h>
#include <LibUI/Event.h>
#include <LibUI/Graphics.h>
#include <LibUI/Panels.h>
#include <LibUI/Platform.h>
#include <signal.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

namespace DesktopShell {

using namespace UI;
using Gfx::FloatPoint;
using Gfx::FloatRect;
using Gfx::IntRect;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

String copy(StringView text)
{
    return MUST(String::from_view(text));
}

// The shell has no menus of its own to say so: Command-W closes these windows itself.
bool closes_on_command_w(View& view, KeyEvent const& event)
{
    if (event.code != KeyW || !event.has_command() || !view.window())
        return false;
    view.window()->hide();
    return true;
}

// About This Computer: the system's mark, its name and version, and what it's running on.
class AboutView final : public View {
public:
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    bool key_down(KeyEvent const& event) override { return closes_on_command_w(*this, event); }

    AboutView()
    {
        utsname name {};
        uname(&name);
        StringBuilder version;
        StringBuilder build;
        build.append(StringView { name.release, strlen(name.release) });
        auto build_text = MUST(build.to_string());
        StringBuilder build_name;
        for (char c : build_text.view())
            build_name.append(c == '.' ? 'A' : c);
        version.appendff("{} ({})", StringView { name.release, strlen(name.release) }, build_name.view());
        m_version = MUST(version.to_string());
        int processors = 1;
        size_t size = sizeof(processors);
        (void)sysctlbyname("hw.ncpu", &processors, &size, nullptr, 0);
        unsigned long long memory = 0;
        size = sizeof(memory);
        (void)sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);
        StringBuilder chip;
        chip.appendff("{}, {} cores", StringView { name.machine, strlen(name.machine) }, processors);
        m_rows.append({ copy("Chip"_sv), MUST(chip.to_string()) });
        StringBuilder memory_text;
        memory_text.appendff("{} GB", static_cast<unsigned long long>((memory + (512ull << 20)) >> 30));
        m_rows.append({ copy("Memory"_sv), MUST(memory_text.to_string()) });
        m_rows.append({ copy("Startup disk"_sv), copy("Vulture HD"_sv) });
        m_rows.append({ copy("Computer name"_sv), copy(StringView { name.nodename, strlen(name.nodename) }) });
        m_rows.append({ copy("vultureOS"_sv), copy(m_version.view()) });
        auto& more = add<Button>("More Info\xE2\x80\xA6"_sv);
        more.on_action = [] {
            auto bundle = Desktop::read_bundle("/System/Applications/Settings.app"_sv);
            StringView arguments[1] = { "About"_sv };
            if (!bundle.is_error())
                (void)Desktop::launch(bundle.value(), Span<StringView const>(arguments, 1));
        };
        m_more = &more;
    }

    void layout() override
    {
        auto const& t = tokens();
        auto const button = m_more->intrinsic_size();
        m_more->set_frame({ (width() - button.width) / 2, t.metric("about.buttonTop"_sv), button.width, button.height });
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const logo = t.metric("about.logoSize"_sv);
        FloatRect const mark { (width() - logo) / 2, t.metric("about.logoTop"_sv), logo, logo };
        graphics.fill_continuous(mark, logo * 0.225f, t.color("about.logoBackground"_sv));
        Desktop::draw_logo(graphics, mark.inflated(-logo * 0.2f), t.color("about.logoMark"_sv));
        float y = mark.y + logo + t.metric("about.nameGap"_sv);
        graphics.draw_text_centered("vultureOS"_sv, t.named_font("aboutName"_sv), { 0, y, width(), 28 }, t.color("label"_sv));
        y += 30;
        StringBuilder subtitle;
        subtitle.appendff("Version {}", m_version.view());
        graphics.draw_text_centered(subtitle.view(), t.named_font("body"_sv), { 0, y, width(), 18 }, t.color("secondaryLabel"_sv));
        y += t.metric("about.rowsGap"_sv);
        auto const body = t.named_font("small"_sv);
        float const middle = width() / 2;
        for (auto const& row : m_rows) {
            // Names to the left of the middle, what they are to the right: as About This Mac sets them.
            float const name_width = Appearance::text_width(body, row.name.view());
            graphics.draw_text(row.name.view(), body, { middle - 6 - name_width, y + 12 }, t.color("label"_sv));
            graphics.draw_text(row.value.view(), body, { middle + 6, y + 12 }, t.color("secondaryLabel"_sv));
            y += t.metric("about.rowHeight"_sv);
        }
        graphics.draw_text_centered("\xE2\x84\xA2 and \xC2\xA9 2022\xE2\x80\x93" "2026 Krisna Pranav."_sv, t.named_font("mini"_sv), { 0, height() - 30, width(), 14 }, t.color("secondaryLabel"_sv));
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return copy("About This Computer"_sv); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (auto const& row : m_rows)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::StaticText, row.name.view(), {}, row.value.view()));
    }

private:
    struct Row {
        String name;
        String value;
    };
    Vector<Row> m_rows;
    String m_version;
    Button* m_more { nullptr };
};

}

void AboutWindow::show()
{
    auto const& t = tokens();
    if (!m_window) {
        auto const screen = Platform::the().screen_size();
        int const width = static_cast<int>(t.metric("about.width"_sv));
        int const height = static_cast<int>(t.metric("about.height"_sv));
        m_window = adopt_own(*new UI::Window("About This Computer"_sv, { (screen.width - width) / 2, (screen.height - height) / 3, width, height }, WS::WindowTitled));
        auto& view = m_window->set_content<AboutView>();
        m_window->make_first_responder(&view);
    }
    m_window->show();
    m_window->backend().activate();
}

// The applications running, by name and icon: one chosen to be stopped.
class ForceQuitList final : public View {
public:
    void set(Vector<Desktop::ApplicationEntry> const& applications)
    {
        i32 const chosen_pid = chosen() ? chosen()->pid : 0;
        m_rows.clear();
        for (auto const& application : applications) {
            Row row;
            row.pid = application.pid;
            row.name = copy(application.name.view());
            row.not_responding = application.not_responding;
            auto bundle = Desktop::application_with_identifier(application.identifier.view());
            row.icon = copy(bundle.has_value() ? bundle.value().icon.view() : "GenericApplication"_sv);
            m_rows.append(move(row));
        }
        m_selected = -1;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].pid == chosen_pid)
                m_selected = static_cast<int>(i);
        }
        set_needs_display();
        if (on_selection_change)
            on_selection_change();
    }

    struct Row {
        i32 pid { 0 };
        String name;
        String icon;
        bool not_responding { false };
    };
    [[nodiscard]] Row const* chosen() const { return m_selected >= 0 && static_cast<size_t>(m_selected) < m_rows.size() ? &m_rows[static_cast<size_t>(m_selected)] : nullptr; }
    Function<void()> on_selection_change;

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill({ 0, 0, width(), height() }, t.color("controlBackground"_sv));
        graphics.stroke_rounded({ 0, 0, width(), height() }, 0, graphics.pixel(), t.color("separator"_sv));
        float const row = t.metric("forceQuit.rowHeight"_sv);
        float const icon = t.metric("forceQuit.iconSize"_sv);
        auto const font = t.named_font("body"_sv);
        for (size_t i = 0; i < m_rows.size(); ++i) {
            auto const& entry = m_rows[i];
            FloatRect const place { 0, static_cast<float>(i) * row, width(), row };
            bool const selected = static_cast<int>(i) == m_selected;
            if (selected)
                graphics.fill(place, t.color("selectedContent"_sv));
            if (auto bitmap = Desktop::icon_named(entry.icon.view(), static_cast<int>(icon), static_cast<int>(graphics.scale())))
                graphics.painter().draw_bitmap(*bitmap, FloatRect { 8, place.y + (row - icon) / 2, icon, icon });
            float const baseline = place.y + (row + Appearance::cap_height(font)) / 2;
            auto const text_color = selected ? t.color("selectedContentText"_sv) : t.color("label"_sv);
            graphics.draw_text(entry.name.view(), font, { 8 + icon + 8, baseline }, text_color);
            // Not answering: said in red, as on a Mac.
            if (entry.not_responding) {
                float const name_width = Appearance::text_width(font, entry.name.view());
                float const space = Appearance::text_width(font, " "_sv);
                graphics.draw_text("(Not Responding)"_sv, font, { 8 + icon + 8 + name_width + space, baseline }, selected ? text_color : t.color("systemRed"_sv));
            }
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        float const row = tokens().metric("forceQuit.rowHeight"_sv);
        int const index = static_cast<int>(event.position.y / row);
        m_selected = index >= 0 && static_cast<size_t>(index) < m_rows.size() ? index : -1;
        set_needs_display();
        if (on_selection_change)
            on_selection_change();
        return true;
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Table; }
    [[nodiscard]] String accessibility_label() const override { return copy("Applications"_sv); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        float const row = tokens().metric("forceQuit.rowHeight"_sv);
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            // What it says in red, said here too.
            StringView value = static_cast<int>(i) == m_selected ? "selected"_sv : StringView {};
            if (m_rows[i].not_responding)
                value = static_cast<int>(i) == m_selected ? "selected, not responding"_sv : "not responding"_sv;
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Row, m_rows[i].name.view(), { 0, static_cast<float>(i) * row, width(), row }, value));
        }
    }

private:
    Vector<Row> m_rows;
    int m_selected { -1 };
};

namespace {

class ForceQuitView final : public View {
public:
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    bool key_down(KeyEvent const& event) override { return closes_on_command_w(*this, event); }

    ForceQuitView(Function<void()> force_quit)
    {
        m_list = &add<ForceQuitList>();
        m_button = &add<Button>("Force Quit"_sv, ButtonStyle::Default);
        m_button->on_action = move(force_quit);
        m_button->set_enabled(false);
        m_list->on_selection_change = [this] { m_button->set_enabled(m_list->chosen() != nullptr); };
    }

    [[nodiscard]] ForceQuitList& list() { return *m_list; }

    void layout() override
    {
        auto const& t = tokens();
        float const padding = t.metric("forceQuit.padding"_sv);
        float const top = t.metric("forceQuit.listTop"_sv);
        float const bottom = t.metric("forceQuit.listBottom"_sv);
        m_list->set_frame({ padding, top, width() - padding * 2, height() - top - bottom });
        auto const button = m_button->intrinsic_size();
        m_button->set_frame({ width() - padding - button.width, height() - padding - button.height, button.width, button.height });
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const padding = t.metric("forceQuit.padding"_sv);
        auto const font = t.named_font("small"_sv);
        graphics.draw_text("If an app doesn\xE2\x80\x99t respond for a while, select its name and click Force Quit."_sv, font, { padding, padding + 10 }, t.color("label"_sv));
        float const footer_y = height() - t.metric("forceQuit.listBottom"_sv) + 18;
        graphics.draw_text("You can open this window by pressing Command-Option-Escape."_sv, font, { padding, footer_y }, t.color("secondaryLabel"_sv));
    }

private:
    ForceQuitList* m_list { nullptr };
    Button* m_button { nullptr };
};

}

void ForceQuitWindow::show()
{
    auto const& t = tokens();
    if (!m_window) {
        auto const screen = Platform::the().screen_size();
        int const width = static_cast<int>(t.metric("forceQuit.width"_sv));
        int const height = static_cast<int>(t.metric("forceQuit.height"_sv));
        m_window = adopt_own(*new UI::Window("Force Quit Applications"_sv, { (screen.width - width) / 2, (screen.height - height) / 3, width, height }, WS::WindowTitled));
        auto& view = m_window->set_content<ForceQuitView>([this] { force_quit_chosen(); });
        m_list = &view.list();
        m_window->make_first_responder(&view);
    }
    m_list->set(m_applications);
    m_window->show();
    m_window->backend().activate();
}

void ForceQuitWindow::set_state(Desktop::ShellState const& state)
{
    m_applications.clear();
    for (auto const& application : state.applications) {
        Desktop::ApplicationEntry entry;
        entry.pid = application.pid;
        entry.name = copy(application.name.view());
        entry.identifier = copy(application.identifier.view());
        entry.not_responding = application.not_responding;
        m_applications.append(move(entry));
    }
    if (m_list && m_window && m_window->is_shown())
        m_list->set(m_applications);
}

void ForceQuitWindow::force_quit_chosen()
{
    auto const* chosen = m_list ? m_list->chosen() : nullptr;
    if (!chosen)
        return;
    i32 const pid = chosen->pid;
    StringBuilder message;
    message.appendff("Do you want to force \xE2\x80\x9C{}\xE2\x80\x9D to quit?", chosen->name.view());
    auto* alert = new Alert(message.view(), "You will lose any unsaved changes."_sv);
    alert->add_button("Force Quit"_sv, true);
    alert->add_button("Cancel"_sv);
    alert->set_symbol(Symbol::Question);
    alert->show(*m_window, [pid, alert](size_t answer) {
        if (answer == 0)
            (void)kill(pid, SIGKILL);
        Platform::the().defer([alert] { delete alert; });
    });
}

}
