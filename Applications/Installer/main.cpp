/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <InstallerEndpoint.h>
#include <LibDesktop/Desktop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Panels.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

// Puts vultureOS on a disk: Introduction, Destination, Installation, Summary, down the side as macOS's
// has it. Everything that touches a disk is done by installd, which is allowed to, and what's put there
// is what this was booted from.

using namespace UI;
using Gfx::FloatRect;
using Gfx::FloatSize;
using Gfx::IntRect;

namespace {

Appearance const& tokens() { return Appearance::the(); }

constexpr size_t step_count = 4;
constexpr StringView step_names[step_count] = { "Introduction"_sv, "Destination"_sv, "Installation"_sv, "Summary"_sv };

// The steps, with a dot for each: the one you're on in the accent, and what's been done a darker gray.
class Steps final : public View {
public:
    void set_current(size_t step)
    {
        m_current = step;
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const row = t.metric("installer.stepHeight"_sv);
        float const dot = t.metric("installer.stepDot"_sv);
        float const left = t.metric("installer.stepInset"_sv);
        float const top = t.metric("installer.stepTop"_sv);
        auto const font = t.named_font("body"_sv);
        auto const strong = t.named_font("headline"_sv);
        for (size_t i = 0; i < step_count; ++i) {
            float const y = top + static_cast<float>(i) * row;
            bool const is_current = i == m_current;
            auto const color = is_current ? t.color("accent"_sv) : t.color(i < m_current ? "secondaryLabel"_sv : "tertiaryLabel"_sv);
            graphics.fill_circle({ left + dot / 2, y + row / 2 }, dot / 2, color);
            graphics.draw_text_centered(step_names[i], is_current ? strong : font, { left + dot + 10, y, width() - left - dot - 10, row },
                t.color(is_current ? "label"_sv : "secondaryLabel"_sv), TextAlignment::Left);
        }
    }

private:
    size_t m_current { 0 };
};

// A disk to choose: all of it can be clicked.
class DiskRow final : public View {
public:
    DiskRow(String name, u64 bytes, Function<void()> on_click)
        : m_name(move(name))
        , m_bytes(bytes)
        , m_on_click(move(on_click))
    {
    }

    void set_selected(bool selected)
    {
        m_selected = selected;
        set_needs_display();
    }

    [[nodiscard]] FloatSize intrinsic_size() const override { return { -1, tokens().metric("installer.diskRowHeight"_sv) }; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const radius = t.metric("installer.diskRowRadius"_sv);
        if (m_selected)
            graphics.fill_rounded({ 0, 0, width(), height() }, radius, t.color("accent"_sv));
        Gfx::Color const white { 255, 255, 255, 255 };
        auto const text = m_selected ? white : t.color("label"_sv);
        auto const secondary = m_selected ? white : t.color("secondaryLabel"_sv);
        float const inset = 12;
        graphics.draw_symbol(Symbol::Drive, { inset, (height() - 28) / 2, 28, 28 }, text, 1.6f);
        graphics.draw_text_centered(m_name.view(), t.named_font("headline"_sv), { inset + 40, 8, width() - inset - 50, height() / 2 - 6 }, text, TextAlignment::Left);
        StringBuilder size;
        // In thousands, as disks are sold and macOS counts.
        u64 const megabytes = m_bytes / 1'000'000;
        if (megabytes >= 1000)
            size.appendff("{}.{} GB", (megabytes + 50) / 1000, ((megabytes + 50) % 1000) / 100);
        else
            size.appendff("{} MB", megabytes);
        graphics.draw_text_centered(size.view(), t.named_font("small"_sv), { inset + 40, height() / 2, width() - inset - 50, height() / 2 - 6 }, secondary, TextAlignment::Left);
    }

    bool mouse_down(MouseEvent const&) override
    {
        if (m_on_click)
            m_on_click();
        return true;
    }

private:
    String m_name;
    u64 m_bytes;
    Function<void()> m_on_click;
    bool m_selected { false };
};

class InstallerContent final : public View {
public:
    void draw(Graphics& graphics) override
    {
        auto const sidebar = tokens().metric("installer.sidebarWidth"_sv);
        graphics.fill({ sidebar, 0, width() - sidebar, height() }, tokens().color("windowBackground"_sv));
    }

    void layout() override
    {
        for (auto const& child : children())
            child->set_frame({ 0, 0, width(), height() });
    }
};

class InstallerApplication final : public InstallerEventsStub {
public:
    InstallerApplication()
    {
        auto const& t = tokens();
        int const width = static_cast<int>(t.metric("installer.width"_sv));
        int const height = static_cast<int>(t.metric("installer.height"_sv));
        float const sidebar = t.metric("installer.sidebarWidth"_sv);
        auto& window = Application::the().make_window<UI::Window>("Install vultureOS"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowUnifiedTitle);
        m_window = &window;
        window.center_on_screen();
        // It can't be left half done: the disk would start nothing, with nobody told.
        window.on_close_request = [this] { return m_step != 2; };
        window.set_transparent(true);
        window.set_sidebar_material(sidebar);
        window.set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& root = window.set_content<InstallerContent>();
        auto& split = root.add<StackView>(Axis::Horizontal, 0, Alignment::Fill);
        m_steps = &split.add<Steps>();
        m_steps->set_fixed_width(sidebar);

        auto& right = split.add<StackView>(Axis::Vertical, 0, Alignment::Fill);
        right.set_stretch(1, 1);
        float const padding = t.metric("installer.padding"_sv);
        m_page = &right.add<StackView>(Axis::Vertical, 12, Alignment::Fill, Insets { padding + t.metric("window.toolbarCompact"_sv), padding, padding, padding });
        m_page->set_stretch(1, 1);
        (void)right.add<Separator>();
        auto& buttons = right.add<StackView>(Axis::Horizontal, 10, Alignment::Center, Insets { 12, padding, 12, padding });
        buttons.set_fixed_height(t.metric("installer.buttonBarHeight"_sv));
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        m_back = &buttons.add<Button>("Go Back"_sv);
        m_back->set_control_size(ControlSize::Large);
        m_back->on_action = [this] { go_back(); };
        m_continue = &buttons.add<Button>("Continue"_sv, ButtonStyle::Default);
        m_continue->set_control_size(ControlSize::Large);
        m_continue->on_action = [this] { go_on(); };

        connect();
        show_step(0);
        window.show();
    }

    void changed(IPC::Sender const&) override { }

    void progress(IPC::Sender const&, String, u32 percent) override
    {
        if (m_progress)
            m_progress->set_value(static_cast<float>(percent) / 100);
    }

    void finished(IPC::Sender const&, String name, bool succeeded, String message) override
    {
        if (name.view() != "system"_sv)
            return;
        m_succeeded = succeeded;
        m_message = move(message);
        printf("installer: %s\n", succeeded ? "installed" : "failed");
        fflush(stdout);
        show_step(3);
    }

private:
    void connect()
    {
        auto port = IPC::look_up("com.vulture.installd"_sv);
        if (port.is_error())
            return;
        m_installer = adopt_own(*new InstallerProxy(port.release_value()));
        auto pair = IPC::create_port();
        if (pair.is_error())
            return;
        auto ports = pair.release_value();
        m_listener = adopt_own(*new IPC::Listener(Application::the().event_loop(), move(ports.receive), [this](IPC::ReceivedMessage& message) { (void)handle(message); }));
        (void)m_installer->subscribe(move(ports.send));
    }

    void clear_page()
    {
        m_rows.clear();
        m_progress = nullptr;
        m_page->remove_all_children();
    }

    Label& add_title(StringView text)
    {
        return m_page->add<Label>(text, tokens().named_font("title2"_sv));
    }

    Label& add_text(StringView text)
    {
        auto& label = m_page->add<Label>(text);
        label.set_wraps(true);
        return label;
    }

    void show_step(size_t step)
    {
        m_step = step;
        m_steps->set_current(step);
        clear_page();
        switch (step) {
        case 0:
            add_title("Welcome to vultureOS"_sv);
            add_text("This installs vultureOS on a disk in this machine, so that it can start up on its own."_sv);
            add_text("Next, you'll choose a disk. Everything on it will be erased."_sv);
            m_back->set_enabled(false);
            m_continue->set_title("Continue"_sv);
            m_continue->set_enabled(true);
            printf("installer: introduction\n");
            break;
        case 1:
            show_destination();
            printf("installer: destination\n");
            break;
        case 2: {
            add_title("Installing vultureOS"_sv);
            add_text("This won't take long. Don't turn off the machine."_sv);
            m_progress = &m_page->add<ProgressBar>();
            m_back->set_enabled(false);
            m_continue->set_enabled(false);
            printf("installer: installing\n");
            break;
        }
        default:
            if (m_succeeded) {
                add_title("vultureOS Has Been Installed"_sv);
                add_text("Remove the disk you started from, then restart."_sv);
                m_back->set_enabled(false);
                m_continue->set_title("Restart"_sv);
                m_continue->set_enabled(true);
            } else {
                add_title("vultureOS Couldn't Be Installed"_sv);
                add_text(m_message.view());
                m_back->set_enabled(true);
                m_continue->set_enabled(false);
            }
            break;
        }
        fflush(stdout);
        auto& filler = m_page->add<View>();
        filler.set_stretch(0, 1);
        m_page->set_needs_layout();
        m_page->set_needs_display();
    }

    void show_destination()
    {
        add_title("Select a Disk"_sv);
        add_text("vultureOS will erase the disk you choose, and install itself on it."_sv);
        m_selected_disk = -1;
        m_continue->set_title("Continue"_sv);
        m_continue->set_enabled(false);
        m_back->set_enabled(true);

        m_disks.clear();
        if (m_installer) {
            auto answer = m_installer->disks();
            if (!answer.is_error()) {
                auto parsed = JSON::parse(answer.value().view());
                if (!parsed.is_error() && parsed.value().is_array()) {
                    for (size_t i = 0; i < parsed.value().size(); ++i) {
                        auto const& entry = parsed.value().at(i);
                        auto const name = entry.get_string("name"_sv);
                        auto const bytes = entry.get_integer("bytes"_sv);
                        if (name.has_value() && bytes.has_value())
                            m_disks.append({ MUST(String::from_view(name.value())), static_cast<u64>(bytes.value()) });
                    }
                }
            }
        }
        if (m_disks.is_empty()) {
            auto& none = add_text("There isn't a disk to install on: what this started from can't be erased while it's in use."_sv);
            none.set_style(LabelStyle::Secondary);
            return;
        }
        for (size_t i = 0; i < m_disks.size(); ++i) {
            auto& row = m_page->add<DiskRow>(MUST(String::from_view(m_disks[i].name.view())), m_disks[i].bytes, [this, i] { select_disk(static_cast<int>(i)); });
            m_rows.append(&row);
        }
    }

    void select_disk(int index)
    {
        m_selected_disk = index;
        for (size_t i = 0; i < m_rows.size(); ++i)
            m_rows[i]->set_selected(static_cast<int>(i) == index);
        m_continue->set_enabled(true);
    }

    void go_back()
    {
        if (m_step > 0)
            show_step(m_step == 3 ? 1 : m_step - 1);
    }

    void go_on()
    {
        if (m_step == 0) {
            show_step(1);
            return;
        }
        if (m_step == 1 && m_selected_disk >= 0) {
            StringBuilder headline;
            headline.appendff("Erase \xE2\x80\x9C{}\xE2\x80\x9D and install vultureOS?", m_disks[static_cast<size_t>(m_selected_disk)].name.view());
            auto* alert = new Alert(headline.view(), "Everything on this disk will be lost. This can't be undone."_sv);
            alert->add_button("Cancel"_sv);
            alert->add_button("Erase and Install"_sv, true);
            printf("installer: asking\n");
            fflush(stdout);
            alert->show(*m_window, [this, alert](size_t button) {
                if (button == 1)
                    begin_installing();
                delete alert;
            });
            return;
        }
        if (m_step == 3 && m_succeeded)
            Desktop::restart();
    }

    void begin_installing()
    {
        show_step(2);
        (void)m_installer->install_system(m_disks[static_cast<size_t>(m_selected_disk)].name.view());
    }

    struct DiskChoice {
        String name;
        u64 bytes;
    };

    UI::Window* m_window { nullptr };
    Steps* m_steps { nullptr };
    StackView* m_page { nullptr };
    Button* m_back { nullptr };
    Button* m_continue { nullptr };
    ProgressBar* m_progress { nullptr };
    Vector<DiskRow*> m_rows;
    Vector<DiskChoice> m_disks;
    int m_selected_disk { -1 };
    size_t m_step { 0 };
    bool m_succeeded { false };
    String m_message;
    OwnPtr<InstallerProxy> m_installer;
    OwnPtr<IPC::Listener> m_listener;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Installer"_sv, "com.vulture.Installer"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Installer: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());
    InstallerApplication installer;
    printf("installer: ready\n");
    fflush(stdout);
    return application->exec();
}
