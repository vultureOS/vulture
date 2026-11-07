/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Application.h>
#include <LibUI/Controls.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// Sound, in the menu bar: a speaker that shows how loud it is, and under it the volume, where sound goes, and the way to
// the rest of it, as Sequoia's Sound module has them. A program of its own, so the menu bar has menu extras as a Mac's does.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

class SoundPanelView final : public View {
public:
    SoundPanelView()
    {
        auto const percent = Application::the().output_volume();
        m_volume = &add<Slider>(0.0f, 100.0f, static_cast<float>(percent.value_or(0)));
        m_volume->set_accessibility_label("Volume"_sv);
        m_volume->set_enabled(percent.has_value());
        m_volume->on_action = [this] {
            Application::the().set_output_volume(static_cast<u32>(m_volume->value() + 0.5f));
            if (on_volume_change)
                on_volume_change();
        };
    }

    Function<void()> on_volume_change;
    Function<void()> on_settings;
    Function<void()> on_outside;

    void layout() override
    {
        auto const& t = tokens();
        float const padding = t.metric("soundMenu.padding"_sv);
        m_volume->set_frame({ padding + 22, t.metric("soundMenu.sliderTop"_sv), width() - padding * 2 - 44, m_volume->intrinsic_size().height });
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const padding = t.metric("soundMenu.padding"_sv);
        auto const heading = t.named_font("headline"_sv);
        auto const body = t.named_font("body"_sv);
        auto const label = t.color("label"_sv);
        auto const secondary = t.color("secondaryLabel"_sv);
        graphics.draw_text("Sound"_sv, heading, { padding, padding + 12 }, label);
        // Quiet at one end, loud at the other.
        float const slider_y = t.metric("soundMenu.sliderTop"_sv);
        graphics.draw_symbol(Symbol::Speaker, { padding, slider_y + 1, 14, 14 }, secondary, 1.4f);
        graphics.draw_symbol(Symbol::Speaker, { width() - padding - 16, slider_y, 16, 16 }, secondary, 1.6f);
        float y = t.metric("soundMenu.outputTop"_sv);
        graphics.draw_text("Output"_sv, t.named_font("small"_sv), { padding, y }, secondary);
        y += 8;
        FloatRect const device { padding - 6, y, width() - (padding - 6) * 2, 26 };
        if (m_hovered == 0)
            graphics.fill_rounded(device, 5, t.color("soundMenu.hover"_sv));
        graphics.fill_circle({ padding + 10, device.y + 13 }, 11, t.color("accent"_sv));
        graphics.draw_symbol(Symbol::Speaker, { padding + 3, device.y + 6, 14, 14 }, t.color("soundMenu.onAccent"_sv), 1.4f);
        graphics.draw_text(device_name(), body, { padding + 28, device.y + 18 }, label);
        y = device.y + device.height + 6;
        graphics.fill({ padding, y, width() - padding * 2, graphics.pixel() }, t.color("separator"_sv));
        y += 6;
        FloatRect const settings { padding - 6, y, width() - (padding - 6) * 2, 24 };
        if (m_hovered == 1)
            graphics.fill_rounded(settings, 5, t.color("soundMenu.hover"_sv));
        graphics.draw_text("Sound Settings\xE2\x80\xA6"_sv, body, { padding, settings.y + 17 }, label);
        m_settings_rect = settings;
        m_device_rect = device;
    }

    void mouse_moved(MouseEvent const& event) override
    {
        int const over = m_device_rect.contains(event.position) ? 0 : (m_settings_rect.contains(event.position) ? 1 : -1);
        if (over != m_hovered) {
            m_hovered = over;
            set_needs_display();
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        // It has the pointer while it's open: a press anywhere else is to put it away.
        if (!FloatRect { 0, 0, width(), height() }.contains(event.position) && on_outside)
            on_outside();
        return true;
    }

    void mouse_up(MouseEvent const& event) override
    {
        if (m_settings_rect.contains(event.position) && on_settings)
            on_settings();
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Sound"_sv)); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        parts.append(Accessibility::part_of(*this, Accessibility::Role::MenuItem, device_name(), m_device_rect, "on"_sv));
        parts.append(Accessibility::part_of(*this, Accessibility::Role::MenuItem, "Sound Settings\xE2\x80\xA6"_sv, m_settings_rect));
    }

private:
    [[nodiscard]] static StringView device_name() { return access("/dev/audio0", F_OK) == 0 ? "Virtual Sound Device"_sv : "None"_sv; }

    Slider* m_volume { nullptr };
    FloatRect m_device_rect;
    FloatRect m_settings_rect;
    int m_hovered { -1 };
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    // Not an application: it has no menus, nor a place in the Dock or Cmd-Tab.
    auto application_or_error = Application::create();
    if (application_or_error.is_error()) {
        fprintf(stderr, "SoundMenu: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    // (The theme is looked up each time: a change of appearance replaces it.)

    // The speaker, with as many waves as it's loud.
    Desktop::MenuExtra extra("Sound"_sv, tokens().metric("soundMenu.extraWidth"_sv), [](Graphics& graphics, FloatRect all, Color color) {
        float const size = 16;
        graphics.draw_symbol(Symbol::Speaker, { all.x + (all.width - size) / 2, all.y + (all.height - size) / 2, size, size }, color, 1.6f);
    });

    OwnPtr<UI::Window> panel;
    SoundPanelView* view = nullptr;
    auto close = [&] {
        if (!panel || !panel->is_shown())
            return;
        panel->backend().grab_pointer(false);
        panel->hide();
        extra.set_highlighted(false);
    };
    extra.on_press = [&](IntRect at) {
        if (panel && panel->is_shown()) {
            close();
            return;
        }
        int const width = static_cast<int>(tokens().metric("soundMenu.width"_sv));
        int const height = static_cast<int>(tokens().metric("soundMenu.height"_sv));
        auto const screen = Platform::the().screen_size();
        IntRect const frame { min(at.x, screen.width - width - 8), at.bottom() + 4, width, height };
        if (!panel) {
            panel = adopt_own(*new UI::Window("Sound"_sv, frame, WS::WindowBare | WS::WindowPopup));
            panel->set_transparent(true);
            panel->backend().set_level(WS::LevelPopup);
            int const radius = static_cast<int>(tokens().metric("soundMenu.radius"_sv));
            panel->backend().set_corner_radius(radius);
            MaterialRegion const material { { 0, 0, width, height }, WS::Material::Popover, radius };
            panel->set_materials(Span<MaterialRegion const>(&material, 1));
            view = &panel->root_view().add<SoundPanelView>();
            view->set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
            // It has the pointer while it's open, wherever that goes.
            panel->set_pointer_catcher(view);
            view->on_outside = [&] { close(); };
            view->on_settings = [&] {
                close();
                auto bundle = Desktop::read_bundle("/System/Applications/Settings.app"_sv);
                StringView arguments[1] = { "Sound"_sv };
                if (!bundle.is_error())
                    (void)Desktop::launch(bundle.value(), Span<StringView const>(arguments, 1));
            };
        }
        panel->set_frame(frame);
        panel->show();
        panel->backend().grab_pointer(true);
        extra.set_highlighted(true);
    };
    extra.show();
    printf("soundmenu: ready\n");
    fflush(stdout);
    return application->exec();
}
