/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <InstallerEndpoint.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Panels.h>
#include <LibUI/ScrollView.h>
#include <signal.h>
#include <stdio.h>
#include <sys/port.h>

// What is on this machine, and what else could be: what the signed repositories offer, which
// the installer service (com.vulture.installd) fetches, checks and puts in place.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

// What sits at the row's right: a button, or how far along it is.
class RowBase : public View {
public:
    [[nodiscard]] FloatSize intrinsic_size() const override { return { -1, tokens().metric("software.rowHeight"_sv) }; }

    void layout() override
    {
        auto const& t = tokens();
        float padding = t.metric("software.padding"_sv);
        float right = width() - padding;
        for (auto* button : m_buttons) {
            auto size = button->intrinsic_size();
            right -= size.width;
            button->set_frame({ right, (height() - size.height) / 2, size.width, size.height });
            right -= 8;
        }
        m_text_right = right;
    }

protected:
    Button& add_button(StringView title)
    {
        auto& button = add<Button>(title);
        m_buttons.append(&button);
        return button;
    }

    void draw_row(Graphics& graphics, StringView icon_name, StringView title, StringView subtitle)
    {
        auto const& t = tokens();
        float padding = t.metric("software.padding"_sv);
        float icon_size = t.metric("software.iconSize"_sv);
        float y = (height() - icon_size) / 2;
        auto icon = Desktop::icon_named(icon_name, static_cast<int>(icon_size), static_cast<int>(graphics.scale()));
        if (icon)
            graphics.painter().draw_bitmap(*icon, FloatRect { padding, y, icon_size, icon_size });
        float x = padding + icon_size + 12;
        float room = max(m_text_right - x, 0.0f);
        graphics.draw_text_truncated(title, t.named_font("headline"_sv), { x, height() / 2 - 3 }, room, t.color("label"_sv));
        graphics.draw_text_truncated(subtitle, t.named_font("subheadline"_sv), { x, height() / 2 + 14 }, room, t.color("secondaryLabel"_sv));
        graphics.fill(graphics.snapped({ x, height() - graphics.pixel(), width() - x, graphics.pixel() }), t.color("separator"_sv));
    }

private:
    Vector<Button*> m_buttons;
    float m_text_right { 0 };
};

class Catalog;

class ApplicationRow final : public RowBase {
public:
    ApplicationRow(Desktop::AppInfo info, StringView package, Catalog& catalog);

    void draw(Graphics& graphics) override
    {
        draw_row(graphics, m_info.icon.view(), m_info.name.view(), m_info.kind.is_empty() ? m_info.identifier.view() : m_info.kind.view());
    }

private:
    Desktop::AppInfo m_info;
};

class PackageRow final : public RowBase {
public:
    PackageRow(StringView name, StringView version, StringView summary, bool installed, Catalog& catalog);

    void set_progress(u32 percent)
    {
        StringBuilder text;
        text.appendff("{}%", percent);
        m_action->set_title(text.view());
        m_action->set_enabled(false);
        set_needs_layout();
    }

    [[nodiscard]] StringView name() const { return m_name.view(); }

    void draw(Graphics& graphics) override
    {
        StringBuilder title;
        title.appendff("{}  {}", m_name.view(), m_version.view());
        draw_row(graphics, "Software"_sv, title.view(), m_summary.view());
    }

private:
    String m_name;
    String m_version;
    String m_summary;
    Button* m_action { nullptr };
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

class EventListener final : public InstallerEventsStub {
public:
    Function<void()> on_changed;
    Function<void(StringView, u32)> on_progress;
    Function<void(StringView, bool, StringView)> on_finished;

    void changed(IPC::Sender const&) override
    {
        if (on_changed)
            on_changed();
    }
    void progress(IPC::Sender const&, String name, u32 percent) override
    {
        if (on_progress)
            on_progress(name.view(), percent);
    }
    void finished(IPC::Sender const&, String name, bool succeeded, String message) override
    {
        if (on_finished)
            on_finished(name.view(), succeeded, message.view());
    }
};

class Catalog {
public:
    Catalog()
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("software.width"_sv));
        int height = static_cast<int>(t.metric("software.height"_sv));
        m_window = &Application::the().make_window<UI::Window>("Software"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle);
        auto& window = *m_window;
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
        (void)toolbar.add_button(Symbol::Refresh, [this] { refresh(); }, "Check for Software"_sv);

        m_installed_scroller = &right.add<ScrollView>();
        m_installed_scroller->set_stretch(1, 1);
        m_installed_scroller->set_scrolls_horizontally(false);
        m_installed_rows = &m_installed_scroller->make_document<StackView>(Axis::Vertical);

        m_available_scroller = &right.add<ScrollView>();
        m_available_scroller->set_stretch(1, 1);
        m_available_scroller->set_scrolls_horizontally(false);
        m_available_rows = &m_available_scroller->make_document<StackView>(Axis::Vertical);
        m_available_scroller->set_hidden(true);

        m_nothing = &right.add<StackView>(Axis::Vertical, 8, Alignment::Center, Insets::all(40));
        m_nothing->set_stretch(1, 1);
        m_nothing_title = &m_nothing->add<Label>("No software sources"_sv, t.named_font("headline"_sv));
        m_nothing_text = &m_nothing->add<Label>(StringView {});
        m_nothing_text->set_style(LabelStyle::Secondary);
        m_nothing->set_hidden(true);

        connect_to_installer();
        rebuild();
        window.show();
        sidebar.select_row(0);
    }

    void install(StringView name)
    {
        if (m_installer)
            (void)m_installer->install(name);
    }

    void remove(StringView name)
    {
        if (m_installer)
            (void)m_installer->remove(name);
    }

private:
    void connect_to_installer()
    {
        auto port = IPC::look_up("com.vulture.installd"_sv);
        if (port.is_error())
            return;
        m_installer = adopt_own(*new InstallerProxy(port.release_value()));
        auto pair = IPC::create_port();
        if (pair.is_error())
            return;
        m_listener.on_changed = [this] { rebuild(); };
        m_listener.on_progress = [this](StringView name, u32 percent) {
            for (auto* row : m_package_rows) {
                if (row->name() == name)
                    row->set_progress(percent);
            }
        };
        m_listener.on_finished = [this](StringView name, bool succeeded, StringView message) {
            printf("software: %.*s %s\n", static_cast<int>(name.length()), name.characters_without_null_termination(), succeeded ? "done" : "failed");
            fflush(stdout);
            if (succeeded)
                return;
            // As a sentence: what comes from the library is a phrase, for logs and for pkg.
            StringBuilder sentence;
            if (!message.is_empty()) {
                char const first = message[0] >= 'a' && message[0] <= 'z' ? static_cast<char>(message[0] - 32) : message[0];
                sentence.append({ &first, 1 });
                sentence.append(message.substring_view(1));
                if (!message.ends_with("."_sv))
                    sentence.append("."_sv);
            }
            Alert alert(name.is_empty() ? "Software could not be checked for."_sv : "That couldn't be done."_sv, sentence.view());
            alert.add_button("OK"_sv, true);
            alert.show(*m_window, [](size_t) { });
        };
        m_event_listener = adopt_own(*new IPC::Listener(Application::the().event_loop(), move(pair.value().receive), [this](IPC::ReceivedMessage& received) { (void)m_listener.handle(received); }));
        (void)m_installer->subscribe(move(pair.value().send));
    }

    void refresh()
    {
        if (m_installer)
            (void)m_installer->refresh();
    }

    void rebuild()
    {
        m_installed_rows->remove_all_children();
        m_available_rows->remove_all_children();
        m_package_rows.clear();

        JSON::Value catalog = JSON::Value::object();
        if (m_installer) {
            auto text = m_installer->catalog();
            if (!text.is_error()) {
                auto parsed = JSON::parse(text.value().view());
                if (!parsed.is_error())
                    catalog = parsed.release_value();
            }
        }

        auto package_for_bundle = [&](StringView bundle_path) -> StringView {
            auto const* installed = catalog.get("installed"_sv);
            if (!installed || !installed->is_array())
                return {};
            for (size_t i = 0; i < installed->size(); ++i) {
                auto const bundle = installed->at(i).get_string("bundle"_sv).value_or(""_sv);
                if (!bundle.is_empty() && bundle_path.ends_with(bundle))
                    return installed->at(i).get_string("name"_sv).value_or(""_sv);
            }
            return {};
        };

        auto applications = Desktop::installed_applications();
        for (size_t i = 0; i < applications.size(); ++i) {
            auto package = package_for_bundle(applications[i].bundle_path.view());
            (void)m_installed_rows->add<ApplicationRow>(move(applications[i]), package, *this);
        }

        m_available_count = 0;
        if (auto const* available = catalog.get("available"_sv); available && available->is_array()) {
            for (size_t i = 0; i < available->size(); ++i) {
                auto const& entry = available->at(i);
                auto const name = entry.get_string("name"_sv).value_or(""_sv);
                auto const version = entry.get_string("version"_sv).value_or(""_sv);
                auto const summary = entry.get_string("summary"_sv).value_or(""_sv);
                bool const is_installed = entry.get_boolean("installed"_sv).value_or(false);
                auto& row = m_available_rows->add<PackageRow>(name, version, summary, is_installed, *this);
                m_package_rows.append(&row);
                ++m_available_count;
            }
        }
        m_repository_count = static_cast<size_t>(catalog.get_integer("repositories"_sv).value_or(0));
        m_installed_rows->set_needs_layout();
        m_available_rows->set_needs_layout();
        show(m_page);
    }

    void show(size_t row)
    {
        m_page = row;
        m_title->set_text(row == 0 ? "Installed"_sv : "Available"_sv);
        m_installed_scroller->set_hidden(row != 0);
        bool const nothing_available = row == 1 && m_available_count == 0;
        m_available_scroller->set_hidden(row != 1 || nothing_available);
        m_nothing->set_hidden(!nothing_available);
        if (nothing_available) {
            if (m_repository_count == 0) {
                m_nothing_title->set_text("No software sources"_sv);
                m_nothing_text->set_text("Software comes from signed repositories, listed in /etc/pkg/repositories.json."_sv);
            } else {
                m_nothing_title->set_text("Nothing here yet"_sv);
                m_nothing_text->set_text("Click the arrow to ask the repositories what they have."_sv);
            }
        }
        if (m_installer && !m_asked_already && row == 1 && m_available_count == 0 && m_repository_count > 0) {
            m_asked_already = true;
            refresh();
        }
    }

    UI::Window* m_window { nullptr };
    Label* m_title { nullptr };
    ScrollView* m_installed_scroller { nullptr };
    StackView* m_installed_rows { nullptr };
    ScrollView* m_available_scroller { nullptr };
    StackView* m_available_rows { nullptr };
    StackView* m_nothing { nullptr };
    Label* m_nothing_title { nullptr };
    Label* m_nothing_text { nullptr };
    Vector<PackageRow*> m_package_rows;
    OwnPtr<InstallerProxy> m_installer;
    EventListener m_listener;
    OwnPtr<IPC::Listener> m_event_listener;
    size_t m_page { 0 };
    size_t m_available_count { 0 };
    size_t m_repository_count { 0 };
    bool m_asked_already { false };
};

ApplicationRow::ApplicationRow(Desktop::AppInfo info, StringView package, Catalog& catalog)
    : m_info(move(info))
{
    auto& open = add_button("Open"_sv);
    open.on_action = [this] {
        auto launched = Desktop::launch(m_info);
        if (launched.is_error())
            fprintf(stderr, "Software: cannot open %s\n", m_info.name.characters());
    };
    if (!package.is_empty()) {
        auto& remove = add_button("Remove"_sv);
        auto name = MUST(String::from_view(package));
        remove.on_action = [&catalog, name = move(name)] { catalog.remove(name.view()); };
    }
}

PackageRow::PackageRow(StringView name, StringView version, StringView summary, bool installed, Catalog& catalog)
    : m_name(MUST(String::from_view(name)))
    , m_version(MUST(String::from_view(version)))
    , m_summary(MUST(String::from_view(summary)))
{
    m_action = &add_button(installed ? "Installed"_sv : "Get"_sv);
    m_action->set_enabled(!installed);
    m_action->on_action = [this, &catalog] {
        m_action->set_enabled(false);
        catalog.install(m_name.view());
    };
}

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
    Catalog catalog;
    printf("software: ready\n");
    fflush(stdout);
    return application->exec();
}
