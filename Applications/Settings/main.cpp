/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/privacy.h>
#include <LibIPC/Bootstrap.h>
#include <LibJSON/JSON.h>
#include <NotificationsEndpoint.h>
#include <PrivacyEndpoint.h>
#include <DirectoryEndpoint.h>
#include <ScreenSaver.h>
#include <LibCore/AccountDatabase.h>
#include <Desktop.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <LibCore/Account.h>
#include <LibDiskManagement/Disks.h>
#include <LibLocale/Locale.h>
#include <LibUI/Panels.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Layout.h>
#include <LibUI/Motion.h>
#include <LibUI/ScrollView.h>
#include <notify.h>
#include <Kernel/API/POSIX/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

// Settings: a list of panes down the side, each a stack of grouped rows.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

char const* const s_preferences = "com.vulture.desktop";

// A group of rows, on a card.
class Group final : public View {
public:
    Group()
    {
        auto const& t = tokens();
        float padding = t.metric("settings.groupPadding"_sv);
        m_rows = &add<StackView>(Axis::Vertical, 0, Alignment::Fill, Insets { 4, padding, 4, padding });
    }

    [[nodiscard]] View& rows() { return *m_rows; }

    void layout() override { m_rows->set_frame({ 0, 0, width(), height() }); }
    [[nodiscard]] FloatSize intrinsic_size() const override { return m_rows->preferred_size(); }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float radius = t.metric("settings.groupRadius"_sv);
        graphics.fill_rounded({ 0, 0, width(), height() }, radius, t.color("settings.group"_sv));
        graphics.stroke_rounded({ 0, 0, width(), height() }, radius, graphics.pixel(), t.color("settings.groupBorder"_sv));
    }

private:
    StackView* m_rows { nullptr };
};

// A label at the left, and a control at the right.
class Row final : public StackView {
public:
    Row(StringView title, StringView detail = {})
        : StackView(Axis::Horizontal, 12, Alignment::Center)
    {
        auto const& t = tokens();
        set_fixed_height(t.metric("settings.rowHeight"_sv));
        auto& label = add<Label>(title);
        (void)label;
        auto& space = add<View>();
        space.set_stretch(1, 0);
        if (!detail.is_empty()) {
            auto& value = add<Label>(detail);
            value.set_style(LabelStyle::Secondary);
        }
    }
};

class Page final : public StackView {
public:
    Page()
        : StackView(Axis::Vertical, 14, Alignment::Fill, Insets { 18, 24, 18, 24 })
    {
    }

    Group& add_group(StringView heading = {})
    {
        if (!heading.is_empty())
            (void)add<Label>(heading, tokens().named_font("headline"_sv));
        return add<Group>();
    }

    void finish()
    {
        auto& rest = add<View>();
        rest.set_stretch(0, 1);
    }
};

class SwatchRow final : public View {
public:
    struct Swatch {
        char const* name;
        Color color;
    };

    SwatchRow()
    {
        m_swatches.append(Swatch { "Blue", Color { 0, 122, 255, 255 } });
        m_swatches.append(Swatch { "Purple", Color { 175, 82, 222, 255 } });
        m_swatches.append(Swatch { "Pink", Color { 255, 45, 85, 255 } });
        m_swatches.append(Swatch { "Red", Color { 255, 59, 48, 255 } });
        m_swatches.append(Swatch { "Orange", Color { 255, 149, 0, 255 } });
        m_swatches.append(Swatch { "Yellow", Color { 255, 204, 0, 255 } });
        m_swatches.append(Swatch { "Green", Color { 40, 205, 65, 255 } });
        m_swatches.append(Swatch { "Graphite", Color { 142, 142, 147, 255 } });
    }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        auto const& t = tokens();
        float swatch = t.metric("settings.swatch"_sv);
        return { static_cast<float>(m_swatches.size()) * (swatch + 8), swatch };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float swatch = t.metric("settings.swatch"_sv);
        auto accent = Appearance::the().accent();
        for (size_t i = 0; i < m_swatches.size(); ++i) {
            FloatPoint center { static_cast<float>(i) * (swatch + 8) + swatch / 2, swatch / 2 };
            graphics.fill_circle(center, swatch / 2, m_swatches[i].color);
            if (m_swatches[i].color == accent)
                graphics.fill_circle(center, 3.5f, Color { 255, 255, 255, 255 });
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        float swatch = tokens().metric("settings.swatch"_sv);
        auto index = static_cast<int>(event.position.x / (swatch + 8));
        if (index < 0 || static_cast<size_t>(index) >= m_swatches.size())
            return false;
        Appearance::the().set_accent(m_swatches[static_cast<size_t>(index)].color);
        set_needs_display();
        return true;
    }

private:
    Vector<Swatch> m_swatches;
};

class WallpaperPicker final : public View {
public:
    // One row of them; the one chosen is the same in every row.
    explicit WallpaperPicker(Span<StringView const> names)
    {
        for (auto name : names)
            m_names.append(MUST(String::from_view(name)));
        Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
        s_selected = MUST(String::from_view(preferences.string("wallpaper"_sv).value_or("Dusk"_sv)));
        s_pickers.append(this);
    }

    ~WallpaperPicker() override
    {
        s_pickers.remove_all_matching([this](auto* one) { return one == this; });
    }

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        auto const& t = tokens();
        float tile_width = t.metric("settings.wallpaperWidth"_sv);
        float tile_height = t.metric("settings.wallpaperHeight"_sv);
        for (size_t i = 0; i < m_names.size(); ++i) {
            FloatRect tile { static_cast<float>(i) * (tile_width + 10) + 4, 4, tile_width, tile_height };
            parts.append(Accessibility::part_of(*this, Accessibility::Role::RadioButton, m_names[i].view(), tile, m_names[i].view() == s_selected.view() ? "on"_sv : "off"_sv));
        }
    }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        auto const& t = tokens();
        return { static_cast<float>(m_names.size()) * (t.metric("settings.wallpaperWidth"_sv) + 10) + 8, t.metric("settings.wallpaperHeight"_sv) + 28 };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float tile_width = t.metric("settings.wallpaperWidth"_sv);
        float tile_height = t.metric("settings.wallpaperHeight"_sv);
        // Their colors, from the same file the window server has.
        for (size_t i = 0; i < m_names.size(); ++i) {
            // (Room for the ring, inside its own frame.)
            FloatRect tile { static_cast<float>(i) * (tile_width + 10) + 4, 4, tile_width, tile_height };
            Vector<Color> colors = colors_of(m_names[i].view());
            if (colors.size() >= 2)
                graphics.fill_vertical_gradient(tile, 6, colors.first(), colors.last());
            if (m_names[i].view() == s_selected.view())
                graphics.stroke_rounded(tile.inflated(3), 9, 3, Appearance::the().color("accent"_sv));
            auto font = t.named_font("small"_sv);
            graphics.draw_text_centered(m_names[i].view(), font, { tile.x, tile_height + 8, tile_width, 16 }, t.color("label"_sv));
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        auto const& t = tokens();
        float tile_width = t.metric("settings.wallpaperWidth"_sv);
        auto index = static_cast<int>((event.position.x - 4) / (tile_width + 10));
        if (index < 0 || static_cast<size_t>(index) >= m_names.size())
            return false;
        s_selected = MUST(String::from_view(m_names[static_cast<size_t>(index)].view()));
        (void)Application::the().connection().server().set_wallpaper(s_selected.view());
        Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
        preferences.set("wallpaper"_sv, MUST(JSON::Value::string(s_selected.view())));
        preferences.save();
        for (auto* picker : s_pickers)
            picker->set_needs_display();
        return true;
    }

private:
    [[nodiscard]] static Vector<Color> colors_of(StringView name)
    {
        Vector<Color> colors;
        auto bytes = Platform::the().read_file("/System/Library/Themes/Wallpapers.json"_sv);
        if (bytes.is_error())
            return colors;
        auto json = JSON::parse(StringView(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
        if (json.is_error())
            return colors;
        auto const* entry = json.value().get_object(name);
        if (!entry)
            return colors;
        auto const* list = entry->get_array(Appearance::the().is_dark() ? "dark"_sv : "light"_sv);
        if (!list)
            return colors;
        for (size_t i = 0; i < list->size(); ++i) {
            auto text = list->at(i).as_string();
            auto hex = [&](size_t at) {
                auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
                return static_cast<u8>(digit(text[at]) * 16 + digit(text[at + 1]));
            };
            colors.append(Color { hex(1), hex(3), hex(5), 255 });
        }
        return colors;
    }

    Vector<String> m_names;
    static inline String s_selected;
    static inline Vector<WallpaperPicker*> s_pickers;
};

void build_appearance(Page& page)
{
    auto& group = page.add_group();
    auto& mode_row = group.rows().add<Row>("Appearance"_sv);
    auto& mode = mode_row.add<SegmentedControl>();
    mode.add_segment("Light"_sv);
    mode.add_segment("Dark"_sv);
    mode.set_selected(Appearance::the().is_dark() ? 1 : 0);
    mode.on_change = [&mode](size_t index) {
        (void)Application::the().connection().server().set_appearance(index == 1 ? "Dark"_sv : "Light"_sv);
        Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
        preferences.set("appearance"_sv, MUST(JSON::Value::string(index == 1 ? "Dark"_sv : "Light"_sv)));
        preferences.save();
        (void)mode;
    };
    auto& accent_row = group.rows().add<Row>("Accent color"_sv);
    (void)accent_row.add<SwatchRow>();
    page.finish();
}

void build_wallpaper(Page& page)
{
    static constexpr StringView still[] = { "Dusk"_sv, "Ridge"_sv, "Ember"_sv, "Glacier"_sv, "Graphite"_sv };
    // Their colours go with the time of day.
    static constexpr StringView dynamic[] = { "Horizon"_sv, "Dunes"_sv };
    auto& group = page.add_group("Wallpaper"_sv);
    auto& picker = group.rows().add<WallpaperPicker>(Span<StringView const>(still, array_size(still)));
    picker.set_fixed_height(picker.intrinsic_size().height + 16);
    auto& changing = page.add_group("Dynamic Wallpapers"_sv);
    auto& dynamic_picker = changing.rows().add<WallpaperPicker>(Span<StringView const>(dynamic, array_size(dynamic)));
    dynamic_picker.set_fixed_height(dynamic_picker.intrinsic_size().height + 16);
    page.finish();
}

// What each of a display's corners does, at each corner of a picture of one.
class HotCornersSheet final : public StackView {
public:
    explicit HotCornersSheet(UI::Window& window)
        : StackView(Axis::Vertical, 14, Alignment::Fill, Insets::all(20))
    {
        auto add = [&](StringView title, StringView key, int index) {
            auto& row = this->add<StackView>(Axis::Horizontal, 12, Alignment::Center);
            (void)row.add<Label>(title);
            auto& filler = row.add<View>();
            filler.set_stretch(1, 0);
            auto& popup = row.add<PopUpButton>();
            popup.set_accessibility_label(title);
            popup.add_item("-"_sv);
            popup.add_item("Overview"_sv);
            popup.add_item("Application Windows"_sv);
            popup.add_item("Desktop"_sv);
            popup.add_item("Launcher"_sv);
            popup.add_item("Notification Center"_sv);
            popup.add_item("Start Screen Saver"_sv);
            popup.add_item("Lock Screen"_sv);
            Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
            s_corners[index] = static_cast<i32>(preferences.number(key).value_or(0));
            popup.set_selected_index(s_corners[index]);
            auto const key_string = MUST(String::from_view(key));
            popup.on_change = [key_string, index](int chosen) {
                s_corners[index] = chosen;
                Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
                saved.set(key_string.view(), JSON::Value(static_cast<double>(chosen)));
                saved.save();
                (void)Application::the().connection().server().set_hot_corners(s_corners[0], s_corners[1], s_corners[2], s_corners[3]);
            };
        };
        add("Top left"_sv, "hotCornerTopLeft"_sv, 0);
        add("Top right"_sv, "hotCornerTopRight"_sv, 1);
        add("Bottom left"_sv, "hotCornerBottomLeft"_sv, 2);
        add("Bottom right"_sv, "hotCornerBottomRight"_sv, 3);
        auto& buttons = this->add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& done = buttons.add<Button>("Done"_sv, ButtonStyle::Default);
        done.on_action = [&window] { Sheet::dismiss(window); };
    }

private:
    static inline i32 s_corners[4] {};
};

void show_hot_corners(UI::Window& window)
{
    Sheet::present(window, adopt_own(*new HotCornersSheet(window)), { 420, 260 });
}

// Control Center: which of its modules are in the menu bar too, as Sequoia's pane has it.
void build_control_center(Page& page)
{
    Desktop::Preferences control_center("com.vulture.controlcenter"_sv);
    auto saved = [](StringView key, StringView value) {
        Desktop::Preferences preferences("com.vulture.controlcenter"_sv);
        preferences.set(key, MUST(JSON::Value::string(value)));
        preferences.save();
        notify_post("com.vulture.controlcenter.preferences");
    };
    auto& modules = page.add_group("Control Center Modules"_sv);
    auto& focus_row = modules.rows().add<Row>("Focus"_sv);
    auto& focus = focus_row.add<PopUpButton>();
    focus.set_accessibility_label("Focus"_sv);
    focus.add_item("Show When Active"_sv);
    focus.add_item("Don\xE2\x80\x99t Show in Menu Bar"_sv);
    focus.set_selected_index(control_center.string("focus"_sv).value_or("whenActive"_sv) == "never"_sv ? 1 : 0);
    focus.on_change = [saved](int index) { saved("focus"_sv, index == 1 ? "never"_sv : "whenActive"_sv); };
    auto& sound_row = modules.rows().add<Row>("Sound"_sv);
    auto& sound = sound_row.add<PopUpButton>();
    sound.set_accessibility_label("Sound"_sv);
    sound.add_item("Always Show in Menu Bar"_sv);
    sound.add_item("Don\xE2\x80\x99t Show in Menu Bar"_sv);
    sound.set_selected_index(control_center.string("sound"_sv).value_or("whenActive"_sv) == "always"_sv ? 0 : 1);
    sound.on_change = [saved](int index) { saved("sound"_sv, index == 0 ? "always"_sv : "whenActive"_sv); };
    auto& others = page.add_group("Other Modules"_sv);
    auto& switching_row = others.rows().add<Row>("Fast User Switching"_sv);
    auto& switching = switching_row.add<Switch>(control_center.string("fastUserSwitching"_sv).value_or("never"_sv) == "always"_sv);
    switching.set_accessibility_label("Show Fast User Switching in Menu Bar"_sv);
    switching.on_action = [saved, &switching] { saved("fastUserSwitching"_sv, switching.is_on() ? "always"_sv : "never"_sv); };
    page.finish();
}

// Focus: what's held back while it's on. Do Not Disturb, for now: banners wait, and are in Notification Center.
void build_focus(Page& page)
{
    Desktop::Preferences focus("com.vulture.focus"_sv);
    auto& group = page.add_group(""_sv);
    auto& row = group.rows().add<Row>("Do Not Disturb"_sv);
    auto& toggle = row.add<Switch>(!focus.string("mode"_sv).value_or(StringView {}).is_empty());
    toggle.set_accessibility_label("Do Not Disturb"_sv);
    toggle.on_action = [&toggle] {
        Desktop::Preferences preferences("com.vulture.focus"_sv);
        preferences.set("mode"_sv, MUST(JSON::Value::string(toggle.is_on() ? "Do Not Disturb"_sv : ""_sv)));
        preferences.save();
        notify_post("com.vulture.focus.preferences");
    };
    (void)group.rows().add<Row>("While it\xE2\x80\x99s on"_sv, "Notifications are silenced, and wait in Notification Center"_sv);
    page.finish();
}

void build_dock(Page& page)
{
    // The Dock's, kept where the Dock keeps them, with a Mac's names: it hears they've changed at once.
    auto dock_saved = [](StringView key, JSON::Value value) {
        Desktop::Preferences dock("com.vulture.dock"_sv);
        dock.set(key, move(value));
        dock.save();
        notify_post("com.vulture.dock.preferences");
    };
    Desktop::Preferences dock("com.vulture.dock"_sv);
    auto& group = page.add_group("Dock"_sv);
    auto& size_row = group.rows().add<Row>("Size"_sv);
    auto& size = size_row.add<Slider>(16.0f, 128.0f, static_cast<float>(dock.number("tilesize"_sv).value_or(static_cast<double>(tokens().metric("dock.iconSize"_sv)))));
    size.set_fixed_width(220);
    size.set_accessibility_label("Size"_sv);
    size.on_action = [dock_saved, &size] { dock_saved("tilesize"_sv, JSON::Value(static_cast<double>(size.value()))); };
    auto& magnification_row = group.rows().add<Row>("Magnification"_sv);
    auto& magnification = magnification_row.add<Switch>(dock.boolean("magnification"_sv).value_or(true));
    magnification.set_accessibility_label("Magnification"_sv);
    magnification.on_action = [dock_saved, &magnification] { dock_saved("magnification"_sv, JSON::Value(magnification.is_on())); };
    auto& position_row = group.rows().add<Row>("Position on screen"_sv);
    auto& position = position_row.add<PopUpButton>();
    position.set_accessibility_label("Position on screen"_sv);
    position.add_item("Left"_sv);
    position.add_item("Bottom"_sv);
    position.add_item("Right"_sv);
    auto const orientation = dock.string("orientation"_sv).value_or("bottom"_sv);
    position.set_selected_index(orientation == "left"_sv ? 0 : (orientation == "right"_sv ? 2 : 1));
    position.on_change = [dock_saved](int index) {
        dock_saved("orientation"_sv, MUST(JSON::Value::string(index == 0 ? "left"_sv : (index == 2 ? "right"_sv : "bottom"_sv))));
    };
    auto dock_switch = [&](StringView title, StringView key, bool fallback) {
        auto& row = group.rows().add<Row>(title);
        auto& toggle = row.add<Switch>(dock.boolean(key).value_or(fallback));
        toggle.set_accessibility_label(title);
        auto const key_string = MUST(String::from_view(key));
        toggle.on_action = [dock_saved, key_string, &toggle] { dock_saved(key_string.view(), JSON::Value(toggle.is_on())); };
    };
    dock_switch("Automatically hide and show the Dock"_sv, "autohide"_sv, false);
    dock_switch("Animate opening applications"_sv, "launchanim"_sv, true);
    dock_switch("Show indicators for open applications"_sv, "show-process-indicators"_sv, true);
    dock_switch("Show suggested and recent apps in Dock"_sv, "show-recents"_sv, true);

    auto saved = [](StringView key, JSON::Value value) {
        Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
        preferences.set(key, move(value));
        preferences.save();
    };
    Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
    auto server = [] -> auto& { return Application::the().connection().server(); };

    auto& stage_group = page.add_group("Desktop & Stage"_sv);
    auto& reveal_row = stage_group.rows().add<Row>("Click wallpaper to reveal desktop"_sv);
    auto& reveal = reveal_row.add<PopUpButton>();
    reveal.set_accessibility_label("Click wallpaper to reveal desktop"_sv);
    reveal.add_item("Always"_sv);
    reveal.add_item("Only in Stage"_sv);
    reveal.set_selected_index(preferences.boolean("clickWallpaperReveals"_sv).value_or(true) ? 0 : 1);
    reveal.on_change = [saved, server](int index) {
        saved("clickWallpaperReveals"_sv, JSON::Value(index == 0));
        (void)server().set_click_wallpaper(index == 0);
    };
    auto& stage_row = stage_group.rows().add<Row>("Stage"_sv);
    auto& stage = stage_row.add<Switch>(preferences.boolean("stage"_sv).value_or(false));
    stage.set_accessibility_label("Stage"_sv);
    stage.on_action = [saved, server, &stage] {
        bool const on = stage.is_on();
        saved("stage"_sv, JSON::Value(on));
        (void)server().set_stage(on);
    };

    // Widgets: on the desktop or not, and in what colours, as the desktop's own settings are.
    auto widgets_saved = [](StringView key, JSON::Value value) {
        Desktop::Preferences widgets("com.vulture.widgets"_sv);
        widgets.set(key, move(value));
        widgets.save();
        notify_post("com.vulture.widgets.preferences");
    };
    Desktop::Preferences widgets("com.vulture.widgets"_sv);
    auto& widgets_group = page.add_group("Widgets"_sv);
    auto& show_row = widgets_group.rows().add<Row>("Show Widgets"_sv);
    auto& on_desktop = show_row.add<Checkbox>("On Desktop"_sv, widgets.boolean("onDesktop"_sv).value_or(true) ? ToggleState::On : ToggleState::Off);
    on_desktop.on_action = [widgets_saved, &on_desktop] { widgets_saved("onDesktop"_sv, JSON::Value(on_desktop.is_on())); };
    auto& style_row = widgets_group.rows().add<Row>("Widget style"_sv);
    auto& style = style_row.add<PopUpButton>();
    style.set_accessibility_label("Widget style"_sv);
    style.add_item("Automatic"_sv);
    style.add_item("Monochrome"_sv);
    style.add_item("Full-color"_sv);
    static constexpr StringView styles[] { "automatic"_sv, "monochrome"_sv, "fullColor"_sv };
    auto const style_now = widgets.string("style"_sv).value_or("automatic"_sv);
    for (int i = 0; i < 3; ++i) {
        if (style_now == styles[i])
            style.set_selected_index(i);
    }
    style.on_change = [widgets_saved](int index) { widgets_saved("style"_sv, MUST(JSON::Value::string(styles[index]))); };

    auto& windows_group = page.add_group("Windows"_sv);
    struct Tiling {
        bool by_edges;
        bool to_menu_bar;
        bool with_option;
        bool margins;
    };
    static Tiling tiling;
    tiling = { preferences.boolean("tileByEdges"_sv).value_or(true), preferences.boolean("tileToMenuBar"_sv).value_or(true), preferences.boolean("tileWithOption"_sv).value_or(true), preferences.boolean("tileMargins"_sv).value_or(true) };
    auto add_tiling = [&](StringView title, StringView key, bool Tiling::* field) {
        auto& row = windows_group.rows().add<Row>(title);
        auto& toggle = row.add<Switch>(tiling.*field);
        toggle.set_accessibility_label(title);
        auto const key_string = MUST(String::from_view(key));
        toggle.on_action = [saved, server, key_string, field, &toggle] {
            bool const on = toggle.is_on();
            tiling.*field = on;
            saved(key_string.view(), JSON::Value(on));
            (void)server().set_tiling(tiling.by_edges, tiling.to_menu_bar, tiling.with_option, tiling.margins);
        };
    };
    add_tiling("Drag windows to screen edges to tile"_sv, "tileByEdges"_sv, &Tiling::by_edges);
    add_tiling("Drag windows to menu bar to fill screen"_sv, "tileToMenuBar"_sv, &Tiling::to_menu_bar);
    add_tiling("Hold \xE2\x8C\xA5 key while dragging windows to tile"_sv, "tileWithOption"_sv, &Tiling::with_option);
    add_tiling("Tiled windows have margins"_sv, "tileMargins"_sv, &Tiling::margins);

    auto& overview_group = page.add_group("Overview"_sv);
    auto& corners_row = overview_group.rows().add<Row>("Hot Corners"_sv);
    auto& corners = corners_row.add<Button>("Hot Corners\xE2\x80\xA6"_sv);
    corners.on_action = [&corners] { show_hot_corners(*corners.window()); };
    page.finish();
}

void build_network(Page& page)
{
    vulture_interface_info interfaces[8];
    size_t size = sizeof(interfaces);
    if (sysctlbyname("net.interfaces", interfaces, &size, nullptr, 0) != 0)
        size = 0;
    bool any = false;
    for (size_t i = 0; i < size / sizeof(interfaces[0]); ++i) {
        auto const& info = interfaces[i];
        if (info.flags & VULTURE_IFF_LOOPBACK)
            continue;
        any = true;
        StringBuilder heading;
        heading.appendff("Ethernet ({})", StringView { info.name, strlen(info.name) });
        auto& group = page.add_group(heading.view());
        (void)group.rows().add<Row>("Status"_sv, info.address ? "Connected"_sv : "Looking for a network"_sv);
        auto address_row = [&](StringView title, unsigned address) {
            if (address == 0)
                return;
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &address, text, sizeof(text));
            (void)group.rows().add<Row>(title, StringView { text, strlen(text) });
        };
        address_row("IP address"_sv, info.address);
        address_row("Subnet mask"_sv, info.netmask);
        address_row("Router"_sv, info.gateway);
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
        (void)group.rows().add<Row>("Hardware address"_sv, StringView { mac, strlen(mac) });
    }
    if (any) {
        // The servers ipconfigd was told of.
        int fd = open("/etc/resolv.conf", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char text[1024];
            ssize_t got = read(fd, text, sizeof(text) - 1);
            close(fd);
            if (got > 0) {
                text[got] = 0;
                StringBuilder servers;
                StringView { text, static_cast<size_t>(got) }.for_each_split_view('\n', SplitBehavior::SkipEmpty, [&](StringView line) {
                    if (line.starts_with("nameserver "_sv)) {
                        if (!servers.view().is_empty())
                            servers.append(", "_sv);
                        servers.append(line.substring_view(11));
                    }
                });
                if (!servers.view().is_empty()) {
                    auto& dns = page.add_group("DNS"_sv);
                    (void)dns.rows().add<Row>("Servers"_sv, servers.view());
                }
            }
        }
    } else {
        auto& group = page.add_group();
        (void)group.rows().add<Row>("Status"_sv, "No network hardware"_sv);
    }
    page.finish();
}

String application_name_from_path(StringView path)
{
    auto const slash = path.find_last('/');
    StringView name = slash.has_value() ? path.substring_view(slash.value() + 1) : path;
    if (name.ends_with(".app"_sv))
        name = name.substring_view(0, name.length() - 4);
    return MUST(String::from_view(name));
}

void build_privacy(Page& page)
{
    auto read_flag = [](char const* name) {
        int value = 0;
        size_t size = sizeof(value);
        return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value != 0;
    };
    auto& security = page.add_group("Security"_sv);
    (void)security.rows().add<Row>("System volume"_sv, read_flag("security.seal.active") ? "Sealed and verified"_sv : "Not sealed"_sv);
    (void)security.rows().add<Row>("Applications"_sv, read_flag("security.code.enforced") ? "Only signed code can run"_sv : "Anything can run"_sv);

    auto& privacy = page.add_group("Files and Folders"_sv);
    bool any = false;
    if (auto port = IPC::look_up("com.vulture.privacyd"_sv); !port.is_error()) {
        auto* service = new PrivacyProxy(port.release_value());
        auto text = service->decisions();
        if (!text.is_error()) {
            auto list = JSON::parse(text.value().view());
            if (!list.is_error() && list.value().is_array()) {
                for (size_t i = 0; i < list.value().size(); ++i) {
                    auto const& entry = list.value().at(i);
                    auto const application = entry.get_string("application"_sv).value_or(""_sv);
                    u32 const which = static_cast<u32>(entry.get_integer("service"_sv).value_or(0));
                    StringView folder = which == VULTURE_PRIVACY_DESKTOP ? "Desktop"_sv : (which == VULTURE_PRIVACY_DOCUMENTS ? "Documents"_sv : "Downloads"_sv);
                    StringBuilder title;
                    if (which == VULTURE_PRIVACY_SCREEN)
                        title.appendff("{}: Screen Recording", application_name_from_path(application).view());
                    else
                        title.appendff("{}: {} folder", application_name_from_path(application).view(), folder);
                    auto& row = privacy.rows().add<Row>(title.view());
                    auto& toggle = row.add<Switch>(entry.get_boolean("allowed"_sv).value_or(false));
                    auto path = MUST(String::from_view(application));
                    toggle.on_action = [service, path = move(path), which, &toggle] {
                        (void)service->set_decision(path.view(), which, toggle.is_on());
                    };
                    any = true;
                }
            }
        }
    }
    if (!any)
        (void)privacy.rows().add<Row>("No application has asked yet"_sv);
    page.finish();
}

void build_sound(Page& page)
{
    auto& output = page.add_group("Output"_sv);
    (void)output.rows().add<Row>("Output device"_sv, access("/dev/audio0"_sv.characters_without_null_termination(), F_OK) == 0 ? "Virtual Sound Device"_sv : "None"_sv);
    auto& volume_row = output.rows().add<Row>("Output volume"_sv);
    auto percent = Application::the().output_volume();
    auto& volume = volume_row.add<Slider>(0.0f, 100.0f, static_cast<float>(percent.value_or(0)));
    volume.set_fixed_width(220);
    volume.set_enabled(percent.has_value());
    volume.on_action = [&volume] { Application::the().set_output_volume(static_cast<u32>(volume.value() + 0.5f)); };

    auto& effects = page.add_group("Sound Effects"_sv);
    struct Effect {
        StringView title;
        StringView sound;
    };
    static constexpr Effect const all_effects[] = {
        { "Alert"_sv, "alert"_sv },
        { "Notification"_sv, "notification"_sv },
        { "Move to Trash"_sv, "trash"_sv },
        { "Log in"_sv, "login"_sv },
    };
    for (auto const& effect : all_effects) {
        auto& row = effects.rows().add<Row>(effect.title);
        auto& play = row.add<Button>("Play"_sv);
        auto sound = effect.sound;
        play.on_action = [sound] { Platform::the().play_sound(sound); };
    }
    // The sound at power-on, as a Mac's Sound settings have it.
    auto& startup_row = effects.rows().add<Row>("Play sound on startup"_sv);
    auto plays = Application::the().startup_sound();
    auto& startup = startup_row.add<Switch>(plays.value_or(false));
    startup.set_accessibility_label("Play sound on startup"_sv);
    startup.set_enabled(plays.has_value());
    startup.on_action = [&startup] { Application::the().set_startup_sound(startup.is_on()); };
    page.finish();
}

// Language & Region. Only for how things are written, for now: the system's own words stay in English until M44.
struct RegionPreview {
    Label* short_date { nullptr };
    Label* number { nullptr };
    Label* full_date { nullptr };
    Label* short_date_and_time { nullptr };
    Label* preview_number { nullptr };
    Label* preview_money { nullptr };
    PopUpButton* first_day { nullptr };
};

RegionPreview s_region_preview;

// Always this moment, so that it's the formats that change: a Thursday.
constexpr i64 region_sample_time = 1768470060; // 2026-01-15 09:41:00 UTC

void show_region_preview(Locale::LocaleData const& locale)
{
    auto const& preview = s_region_preview;
    preview.short_date->set_text(MUST(Locale::format_date(locale, region_sample_time, Locale::Style::Short, Locale::Style::None)).view());
    preview.number->set_text(MUST(Locale::format_number(locale, 1234567.89)).view());
    preview.full_date->set_text(MUST(Locale::format_date(locale, region_sample_time, Locale::Style::Full, Locale::Style::Short)).view());
    preview.short_date_and_time->set_text(MUST(Locale::format_date(locale, region_sample_time, Locale::Style::Short, Locale::Style::Short)).view());
    preview.preview_number->set_text(MUST(Locale::format_number(locale, 1234.56)).view());
    preview.preview_money->set_text(MUST(Locale::format_currency(locale, 4567.89)).view());
}

// What a place is called, in English for now, as the rest of the system is.
StringView region_name_of(Locale::LocaleData const& locale)
{
    auto const& english = Locale::default_locale();
    for (size_t i = 0; i < Locale::region_name_count; ++i) {
        if (StringView(Locale::region_codes[i]) == StringView(locale.region))
            return StringView(english.region_names[i]);
    }
    return StringView(locale.region);
}

void build_language_region(Page& page)
{
    Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
    auto const chosen = preferences.string("region"_sv).value_or("en_US"_sv);
    auto const* found = Locale::find_locale(chosen);
    auto const& locale = found ? *found : Locale::default_locale();

    auto& group = page.add_group();
    auto& region_row = group.rows().add<Row>("Region"_sv);
    auto& regions = region_row.add<PopUpButton>();
    regions.set_accessibility_label("Region"_sv);
    for (size_t i = 0; i < Locale::locale_count; ++i) {
        regions.add_item(region_name_of(Locale::locales[i]));
        if (&Locale::locales[i] == &locale)
            regions.set_selected_index(static_cast<int>(i));
    }
    (void)group.rows().add<Row>("Calendar"_sv, "Gregorian"_sv);
    auto& first_day_row = group.rows().add<Row>("First day of week"_sv);
    auto& first_day = first_day_row.add<PopUpButton>();
    for (auto const* day : Locale::default_locale().days_format_wide)
        first_day.add_item(StringView(day));
    first_day.set_selected_index(static_cast<int>(preferences.number("firstDayOfWeek"_sv).value_or(locale.first_day_of_week)));
    first_day.on_change = [](int index) {
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("firstDayOfWeek"_sv, JSON::Value(static_cast<double>(index)));
        saved.save();
    };
    s_region_preview.first_day = &first_day;
    auto& date_row = group.rows().add<Row>("Date format"_sv);
    auto& short_date = date_row.add<Label>(""_sv);
    short_date.set_style(LabelStyle::Secondary);
    s_region_preview.short_date = &short_date;
    auto& number_row = group.rows().add<Row>("Number format"_sv);
    auto& number = number_row.add<Label>(""_sv);
    number.set_style(LabelStyle::Secondary);
    s_region_preview.number = &number;
    (void)group.rows().add<Row>("List sort order"_sv, "Universal"_sv);

    auto& example = page.add_group("Preview"_sv);
    auto add_line = [&](Label*& into) {
        auto& line = example.rows().add<Label>(""_sv);
        into = &line;
    };
    add_line(s_region_preview.full_date);
    add_line(s_region_preview.short_date_and_time);
    add_line(s_region_preview.preview_number);
    add_line(s_region_preview.preview_money);

    regions.on_change = [](int index) {
        auto const& picked = Locale::locales[index];
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("region"_sv, MUST(JSON::Value::string(StringView(picked.identifier))));
        // What it says for the week goes with the place, as on a Mac, until it's been changed again.
        saved.set("firstDayOfWeek"_sv, JSON::Value(static_cast<double>(picked.first_day_of_week)));
        saved.save();
        s_region_preview.first_day->set_selected_index(picked.first_day_of_week);
        show_region_preview(picked);
    };
    show_region_preview(locale);
    page.finish();
}

void build_keyboard(Page& page)
{
    Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));

    auto& sources = page.add_group("Input Sources"_sv);
    auto& layout_row = sources.rows().add<Row>("Keyboard layout"_sv);
    auto& layouts = layout_row.add<PopUpButton>();
    layouts.set_accessibility_label("Keyboard layout"_sv);
    struct Named {
        StringView file;
        StringView title;
    };
    static constexpr Named all_layouts[] = {
        { "US"_sv, "U.S."_sv }, { "British"_sv, "British"_sv }, { "German"_sv, "German"_sv }, { "French"_sv, "French"_sv }, { "Spanish"_sv, "Spanish"_sv }, { "Italian"_sv, "Italian"_sv },
        { "Dutch"_sv, "Dutch"_sv }, { "Swedish-Pro"_sv, "Swedish"_sv }, { "Polish"_sv, "Polish"_sv }, { "Turkish"_sv, "Turkish"_sv }, { "Brazilian"_sv, "Brazilian"_sv }, { "Russian"_sv, "Russian"_sv },
        { "Greek"_sv, "Greek"_sv }, { "Arabic"_sv, "Arabic"_sv }, { "Hebrew"_sv, "Hebrew"_sv }, { "Devanagari"_sv, "Hindi"_sv },
    };
    auto const chosen_layout = preferences.string("keyboardLayout"_sv).value_or("US"_sv);
    for (size_t i = 0; i < array_size(all_layouts); ++i) {
        layouts.add_item(all_layouts[i].title);
        if (all_layouts[i].file == chosen_layout)
            layouts.set_selected_index(static_cast<int>(i));
    }
    layouts.on_change = [](int index) {
        auto const file = all_layouts[index].file;
        (void)Application::the().connection().server().set_keyboard_layout(file);
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("keyboardLayout"_sv, MUST(JSON::Value::string(file)));
        saved.save();
    };

    auto& method_row = sources.rows().add<Row>("Input method"_sv);
    auto& methods = method_row.add<PopUpButton>();
    methods.set_accessibility_label("Input method"_sv);
    static constexpr Named all_methods[] = { { ""_sv, "None"_sv }, { "Pinyin"_sv, "Pinyin - Simplified"_sv }, { "Kana"_sv, "Japanese - Romaji"_sv }, { "Hangul"_sv, "2-Set Korean"_sv } };
    auto const chosen_method = preferences.string("inputMethod"_sv).value_or(""_sv);
    for (size_t i = 0; i < array_size(all_methods); ++i) {
        methods.add_item(all_methods[i].title);
        if (all_methods[i].file == chosen_method)
            methods.set_selected_index(static_cast<int>(i));
    }
    methods.on_change = [](int index) {
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("inputMethod"_sv, MUST(JSON::Value::string(all_methods[index].file)));
        saved.save();
    };

    auto& group = page.add_group("Keyboard"_sv);
    auto& row = group.rows().add<Row>("Keyboard navigation"_sv);
    auto& toggle = row.add<Switch>(Control::full_keyboard_access());
    toggle.on_action = [&toggle] { Control::set_full_keyboard_access(toggle.is_on()); };
    (void)group.rows().add<Row>("Move focus"_sv, "Tab"_sv);
    (void)group.rows().add<Row>("Search"_sv, "\xE2\x8C\x98Space"_sv);
    (void)group.rows().add<Row>("Overview"_sv, "\xE2\x8C\x83\xE2\x86\x91"_sv);
    (void)group.rows().add<Row>("Switch applications"_sv, "\xE2\x8C\x98Tab"_sv);
    (void)group.rows().add<Row>("Emoji & Symbols"_sv, "\xE2\x8C\x83\xE2\x8C\x98Space"_sv);

    auto& text_input = page.add_group("Text Input"_sv);
    struct Option {
        StringView title;
        StringView key;
        bool on_by_default;
    };
    static constexpr Option options[] = {
        { "Correct spelling automatically"_sv, "correctSpelling"_sv, true },
        { "Capitalize words automatically"_sv, "capitalizeWords"_sv, true },
        { "Add period with double-space"_sv, "periodWithDoubleSpace"_sv, true },
        { "Use text replacements"_sv, "useTextReplacements"_sv, true },
    };
    for (auto const& option : options) {
        auto& option_row = text_input.rows().add<Row>(option.title);
        auto& option_switch = option_row.add<Switch>(preferences.boolean(option.key).value_or(option.on_by_default));
        option_switch.set_accessibility_label(option.title);
        auto const key = option.key;
        option_switch.on_action = [&option_switch, key] {
            Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
            saved.set(key, JSON::Value(option_switch.is_on()));
            saved.save();
        };
    }
    page.finish();
}

// A page of its own, with Back to where it came from: an application's notifications, a person.
Function<void(StringView title, Function<void(Page&)>& build)> s_push_page;

// ---- Users & Groups

// Once an administrator's name and password have been given, Settings may make changes until it quits, as a Mac's does.
struct Authorization {
    String name;
    String password;
    bool given { false };
};
Authorization s_authorization;
// The pane shown, made again: after an account's made or taken away.
Function<void()> s_refresh_page;

OwnPtr<DirectoryProxy> directory()
{
    auto port = IPC::look_up("com.vulture.directory"_sv);
    if (port.is_error())
        return nullptr;
    return adopt_own(*new DirectoryProxy(port.release_value()));
}

StringView why_not(i32 error)
{
    switch (error) {
    case EACCES:
        return "The name or password is incorrect."_sv;
    case EEXIST:
        return "That account name is already used."_sv;
    case EINVAL:
        return "An account name starts with a lower-case letter, and has only those, numbers, \xE2\x80\x9C-\xE2\x80\x9D, \xE2\x80\x9C_\xE2\x80\x9D and \xE2\x80\x9C.\xE2\x80\x9D"_sv;
    case EPERM:
        return "There has to be an administrator."_sv;
    case EBUSY:
        return "That can\xE2\x80\x99t be done to your own account."_sv;
    default:
        return "The change couldn\xE2\x80\x99t be made."_sv;
    }
}

void refresh_soon()
{
    Platform::the().defer([] {
        if (s_refresh_page)
            s_refresh_page();
    });
}

// A label at the left, a field at the right: in a sheet.
TextField& add_sheet_field(StackView& form, StringView title, StringView placeholder, bool secure)
{
    auto& row = form.add<StackView>(Axis::Horizontal, 10, Alignment::Center);
    auto& label = row.add<Label>(title);
    label.set_alignment(TextAlignment::Right);
    label.set_fixed_width(110);
    auto& field = row.add<TextField>();
    field.set_fixed_width(220);
    field.set_placeholder(placeholder);
    field.set_secure(secure);
    field.set_accessibility_label(title);
    return field;
}

// "Settings wants to make changes.": an administrator's name and password, asked for once.
class AuthorizationSheet final : public StackView {
public:
    AuthorizationSheet(UI::Window& window, Function<void()> allowed)
        : StackView(Axis::Vertical, 12, Alignment::Fill, Insets::all(20))
        , m_window(window)
        , m_allowed(move(allowed))
    {
        auto& heading = add<Label>("Settings wants to make changes."_sv, tokens().named_font("headline"_sv));
        (void)heading;
        auto& explanation = add<Label>("Enter an administrator\xE2\x80\x99s name and password to allow this."_sv);
        explanation.set_wraps(true);
        auto& form = add<StackView>(Axis::Vertical, 8, Alignment::Leading);
        m_name = &add_sheet_field(form, "User Name"_sv, {}, false);
        struct passwd const* me = getpwuid(getuid());
        if (me)
            m_name->set_text(StringView(me->pw_name, strlen(me->pw_name)));
        m_password = &add_sheet_field(form, "Password"_sv, {}, true);
        m_problem = &add<Label>(" "_sv);
        m_problem->set_color_token("systemRed"_sv);
        auto& buttons = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& cancel = buttons.add<Button>("Cancel"_sv);
        cancel.on_action = [this] { Sheet::dismiss(m_window); };
        auto& allow = buttons.add<Button>("Modify Settings"_sv, ButtonStyle::Default);
        allow.on_action = [this] { try_allowing(); };
        window.set_default_action([this] { try_allowing(); });
    }

    [[nodiscard]] TextField& password() { return *m_password; }

private:
    void try_allowing()
    {
        auto proxy = directory();
        auto answer = proxy ? proxy->check_administrator(m_name->text(), m_password->text()) : ErrorOr<i32>(Error::from_errno(ENOENT));
        i32 const error = answer.is_error() ? answer.error().code() : answer.value();
        if (error != 0) {
            m_problem->set_text(why_not(error));
            m_password->set_text({});
            printf("settings: not allowed\n");
            fflush(stdout);
            return;
        }
        s_authorization = { MUST(String::from_view(m_name->text())), MUST(String::from_view(m_password->text())), true };
        printf("settings: allowed\n");
        fflush(stdout);
        auto allowed = move(m_allowed);
        auto& window = m_window;
        window.set_default_action(nullptr);
        Sheet::dismiss(window);
        // (This sheet is gone by then.)
        auto* raw = adopt_own(*new Function<void()>(move(allowed))).leak_ptr();
        Platform::the().defer([raw] {
            (*raw)();
            delete raw;
        });
    }

    UI::Window& m_window;
    Function<void()> m_allowed;
    TextField* m_name { nullptr };
    TextField* m_password { nullptr };
    Label* m_problem { nullptr };
};

void with_authorization(UI::Window& window, Function<void()> then)
{
    if (s_authorization.given) {
        then();
        return;
    }
    auto sheet = adopt_own(*new AuthorizationSheet(window, move(then)));
    auto* field = &sheet->password();
    Sheet::present(window, move(sheet), { 420, 250 });
    window.make_first_responder(field);
}

// A new person: Standard or an administrator, their names, a password, and its hint.
class NewAccountSheet final : public StackView {
public:
    explicit NewAccountSheet(UI::Window& window)
        : StackView(Axis::Vertical, 10, Alignment::Fill, Insets::all(20))
        , m_window(window)
    {
        auto& form = add<StackView>(Axis::Vertical, 8, Alignment::Leading);
        auto& kind_row = form.add<StackView>(Axis::Horizontal, 10, Alignment::Center);
        auto& kind_label = kind_row.add<Label>("New Account"_sv);
        kind_label.set_alignment(TextAlignment::Right);
        kind_label.set_fixed_width(110);
        m_kind = &kind_row.add<PopUpButton>();
        m_kind->set_accessibility_label("New Account"_sv);
        m_kind->add_item("Standard"_sv);
        m_kind->add_item("Administrator"_sv);
        m_kind->set_selected_index(0);
        m_full_name = &add_sheet_field(form, "Full Name"_sv, {}, false);
        m_account_name = &add_sheet_field(form, "Account Name"_sv, {}, false);
        m_password = &add_sheet_field(form, "Password"_sv, "Required"_sv, true);
        m_verify = &add_sheet_field(form, "Verify"_sv, "Verify"_sv, true);
        m_hint = &add_sheet_field(form, "Password Hint"_sv, "Recommended"_sv, false);
        m_full_name->on_change = [this] {
            if (m_name_edited)
                return;
            m_filling = true;
            m_account_name->set_text(Core::AccountDatabase::name_from_full_name(m_full_name->text()).view());
            m_filling = false;
        };
        m_account_name->on_change = [this] {
            if (!m_filling)
                m_name_edited = true;
        };
        m_problem = &add<Label>(" "_sv);
        m_problem->set_color_token("systemRed"_sv);
        m_problem->set_wraps(true);
        auto& buttons = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& cancel = buttons.add<Button>("Cancel"_sv);
        cancel.on_action = [this] {
            m_window.set_default_action(nullptr);
            Sheet::dismiss(m_window);
        };
        auto& create = buttons.add<Button>("Create User"_sv, ButtonStyle::Default);
        create.on_action = [this] { try_creating(); };
        window.set_default_action([this] { try_creating(); });
    }

    [[nodiscard]] TextField& first_field() { return *m_full_name; }

private:
    void try_creating()
    {
        if (m_full_name->text().is_empty()) {
            m_problem->set_text("Enter a full name."_sv);
            return;
        }
        if (m_password->text() != m_verify->text()) {
            m_problem->set_text("The passwords don\xE2\x80\x99t match."_sv);
            return;
        }
        auto proxy = directory();
        auto answer = proxy ? proxy->create_user(s_authorization.name.view(), s_authorization.password.view(), m_account_name->text(), m_full_name->text(), m_password->text(), m_hint->text(), m_kind->selected_index() == 1)
                            : ErrorOr<i32>(Error::from_errno(ENOENT));
        i32 const error = answer.is_error() ? answer.error().code() : answer.value();
        if (error != 0) {
            m_problem->set_text(why_not(error));
            return;
        }
        printf("settings: made %.*s\n", static_cast<int>(m_account_name->text().length()), m_account_name->text().characters_without_null_termination());
        fflush(stdout);
        m_window.set_default_action(nullptr);
        Sheet::dismiss(m_window);
        refresh_soon();
    }

    UI::Window& m_window;
    PopUpButton* m_kind { nullptr };
    TextField* m_full_name { nullptr };
    TextField* m_account_name { nullptr };
    TextField* m_password { nullptr };
    TextField* m_verify { nullptr };
    TextField* m_hint { nullptr };
    Label* m_problem { nullptr };
    bool m_name_edited { false };
    bool m_filling { false };
};

// Taking someone away: what's to become of their home folder, as a Mac asks.
class DeleteUserSheet final : public StackView {
public:
    DeleteUserSheet(UI::Window& window, Core::AccountDatabase::Person person)
        : StackView(Axis::Vertical, 12, Alignment::Fill, Insets::all(20))
        , m_window(window)
        , m_person(move(person))
    {
        StringBuilder question;
        question.appendff("Are you sure you want to delete the user account \xE2\x80\x9C{}\xE2\x80\x9D?", m_person.full_name.view());
        auto& heading = add<Label>(question.view(), tokens().named_font("headline"_sv));
        heading.set_wraps(true);
        m_delete_home = &add<RadioButton>("Delete the home folder"_sv, true, 7);
        m_keep_home = &add<RadioButton>("Don\xE2\x80\x99t change the home folder"_sv, false, 7);
        m_problem = &add<Label>(" "_sv);
        m_problem->set_color_token("systemRed"_sv);
        auto& buttons = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& cancel = buttons.add<Button>("Cancel"_sv);
        cancel.on_action = [this] { Sheet::dismiss(m_window); };
        auto& remove = buttons.add<Button>("Delete User"_sv, ButtonStyle::Default);
        remove.on_action = [this] {
            auto proxy = directory();
            auto answer = proxy ? proxy->delete_user(s_authorization.name.view(), s_authorization.password.view(), m_person.name.view(), m_delete_home->is_on())
                                : ErrorOr<i32>(Error::from_errno(ENOENT));
            i32 const error = answer.is_error() ? answer.error().code() : answer.value();
            if (error != 0) {
                m_problem->set_text(why_not(error));
                return;
            }
            printf("settings: took away %s\n", m_person.name.characters());
            fflush(stdout);
            Sheet::dismiss(m_window);
            refresh_soon();
        };
    }

private:
    UI::Window& m_window;
    Core::AccountDatabase::Person m_person;
    RadioButton* m_delete_home { nullptr };
    RadioButton* m_keep_home { nullptr };
    Label* m_problem { nullptr };
};

// Someone: their picture, their name, and whether they administer the computer.
class PersonRow final : public View {
public:
    PersonRow(Core::AccountDatabase::Person person, bool is_me)
        : m_person(move(person))
        , m_is_me(is_me)
    {
        set_fixed_height(tokens().metric("settings.applicationRowHeight"_sv));
    }

    Function<void()> on_choose;

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const picture = t.metric("settings.applicationIcon"_sv);
        Desktop::draw_monogram(graphics, { 0, (height() - picture) / 2, picture, picture }, m_person.full_name.view());
        float const x = picture + 10;
        graphics.draw_text(m_person.full_name.view(), t.named_font("body"_sv), { x, height() / 2 - 2 }, t.color("label"_sv));
        graphics.draw_text(summary().view(), t.named_font("small"_sv), { x, height() / 2 + 13 }, t.color("secondaryLabel"_sv));
        graphics.draw_symbol(Symbol::ChevronRight, { width() - 12, (height() - 12) / 2, 8, 12 }, t.color("tertiaryLabel"_sv), 1.6f);
    }

    void mouse_up(MouseEvent const& event) override
    {
        if (FloatRect { 0, 0, width(), height() }.contains(event.position) && on_choose)
            on_choose();
    }
    bool mouse_down(MouseEvent const&) override { return true; }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::Row; }
    String accessibility_label() const override { return m_person.full_name; }
    String accessibility_value() const override { return summary(); }
    bool accessibility_press() override
    {
        if (on_choose)
            on_choose();
        return true;
    }

private:
    String summary() const
    {
        StringBuilder text;
        text.append(m_person.administrator ? "Admin"_sv : "Standard"_sv);
        if (m_is_me)
            text.append(", you"_sv);
        return MUST(text.to_string());
    }

    Core::AccountDatabase::Person m_person;
    bool m_is_me;
};

void build_person(Page& page, Core::AccountDatabase::Person const& person)
{
    bool const is_me = person.uid == getuid();
    auto& names = page.add_group();
    (void)names.rows().add<Row>("Full Name"_sv, person.full_name.view());
    (void)names.rows().add<Row>("Account Name"_sv, person.name.view());
    auto& admin_row = names.rows().add<Row>("Allow this user to administer this computer"_sv);
    auto& admin = admin_row.add<Switch>(person.administrator);
    admin.set_accessibility_label("Allow this user to administer this computer"_sv);
    auto const name = person.name;
    admin.on_action = [&admin, name] {
        bool const wanted = admin.is_on();
        admin.set_on(!wanted, false);
        with_authorization(*admin.window(), [&admin, name, wanted] {
            auto proxy = directory();
            auto answer = proxy ? proxy->set_administrator(s_authorization.name.view(), s_authorization.password.view(), name.view(), wanted) : ErrorOr<i32>(Error::from_errno(ENOENT));
            if (!answer.is_error() && answer.value() == 0)
                admin.set_on(wanted);
        });
    };
    if (!is_me) {
        auto& removal = page.add_group();
        auto& row = removal.rows().add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        row.set_fixed_height(tokens().metric("settings.rowHeight"_sv));
        auto& filler = row.add<View>();
        filler.set_stretch(1, 0);
        auto& remove = row.add<Button>("Delete User\xE2\x80\xA6"_sv);
        auto copy = person;
        remove.on_action = [&remove, copy] {
            auto* window = remove.window();
            with_authorization(*window, [window, copy] { Sheet::present(*window, adopt_own(*new DeleteUserSheet(*window, copy)), { 420, 200 }); });
        };
    }
    page.finish();
}

void build_users(Page& page)
{
    auto& group = page.add_group();
    auto people = Core::AccountDatabase::people();
    // You first, as a Mac lists them; then everyone else.
    auto const me = getuid();
    for (int pass = 0; pass < 2; ++pass) {
        for (auto const& person : people) {
            if ((person.uid == me) != (pass == 0))
                continue;
            auto& row = group.rows().add<PersonRow>(person, person.uid == me);
            auto copy = person;
            row.on_choose = [copy] {
                Function<void(Page&)> build = [copy](Page& detail) { build_person(detail, copy); };
                if (s_push_page)
                    s_push_page(copy.full_name.view(), build);
            };
        }
    }
    auto& buttons = page.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
    auto& filler = buttons.add<View>();
    filler.set_stretch(1, 0);
    auto& add = buttons.add<Button>("Add User\xE2\x80\xA6"_sv);
    add.on_action = [&add] {
        auto* window = add.window();
        with_authorization(*window, [window] {
            auto sheet = adopt_own(*new NewAccountSheet(*window));
            auto* field = &sheet->first_field();
            Sheet::present(*window, move(sheet), { 440, 268 });
            window->make_first_responder(field);
        });
    };
    page.finish();
}

// ---- Lock Screen, Screen Saver, Login Items

void build_lock_screen(Page& page)
{
    Desktop::Preferences saver("com.vulture.screensaver"_sv);
    auto& group = page.add_group();
    // Minutes, 0 for never: a Mac's choices.
    static constexpr int minutes[] = { 0, 1, 2, 5, 10, 20, 30, 60 };
    static constexpr StringView titles[] = { "Never"_sv, "For 1 minute"_sv, "For 2 minutes"_sv, "For 5 minutes"_sv, "For 10 minutes"_sv, "For 20 minutes"_sv, "For 30 minutes"_sv, "For 1 hour"_sv };
    auto& idle_row = group.rows().add<Row>("Start Screen Saver when inactive"_sv);
    auto& idle = idle_row.add<PopUpButton>();
    idle.set_accessibility_label("Start Screen Saver when inactive"_sv);
    int const chosen = static_cast<int>(saver.number("idleTime"_sv).value_or(20));
    for (size_t i = 0; i < array_size(titles); ++i) {
        idle.add_item(titles[i]);
        if (minutes[i] == chosen)
            idle.set_selected_index(static_cast<int>(i));
    }
    idle.on_change = [](int index) {
        Desktop::Preferences saved("com.vulture.screensaver"_sv);
        saved.set("idleTime"_sv, JSON::Value(static_cast<double>(minutes[index])));
        saved.save();
    };
    auto& ask_row = group.rows().add<Row>("Require password after screen saver begins"_sv);
    auto& ask = ask_row.add<PopUpButton>();
    ask.set_accessibility_label("Require password after screen saver begins"_sv);
    ask.add_item("Immediately"_sv);
    ask.add_item("Never"_sv);
    ask.set_selected_index(saver.boolean("askForPassword"_sv).value_or(true) ? 0 : 1);
    ask.on_change = [](int index) {
        Desktop::Preferences saved("com.vulture.screensaver"_sv);
        saved.set("askForPassword"_sv, JSON::Value(index == 0));
        saved.save();
    };
    page.finish();
}

// A screen saver, small, and its name under it: a ring round the one chosen.
class SaverChoice final : public Control {
public:
    SaverChoice(StringView name, bool chosen)
        : m_name(MUST(String::from_view(name)))
        , m_saver(name)
        , m_chosen(chosen)
    {
    }

    void set_chosen(bool chosen)
    {
        m_chosen = chosen;
        set_needs_display();
    }
    [[nodiscard]] StringView name() const { return m_name.view(); }

    [[nodiscard]] FloatSize intrinsic_size() const override { return { 176, 134 }; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        FloatRect const picture { 2, 2, 172, 104 };
        graphics.save();
        graphics.clip(picture);
        m_saver.draw_in(graphics, picture);
        graphics.restore();
        if (m_chosen)
            graphics.stroke_rounded({ 0.5f, 0.5f, 175, 107 }, 6, 3, t.color("accent"_sv));
        graphics.draw_text_centered(m_name.view(), t.named_font("body"_sv), { 0, 112, 176, 18 }, t.color("label"_sv));
    }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::RadioButton; }
    String accessibility_label() const override { return m_name; }
    String accessibility_value() const override { return MUST(String::from_view(m_chosen ? "on"_sv : "off"_sv)); }

private:
    String m_name;
    Desktop::ScreenSaver m_saver;
    bool m_chosen;
};

void build_screen_saver(Page& page)
{
    Desktop::Preferences saver("com.vulture.screensaver"_sv);
    auto const chosen = saver.string("name"_sv).value_or("Drift"_sv);
    auto& group = page.add_group("Screen Savers"_sv);
    auto& choices = group.rows().add<StackView>(Axis::Horizontal, 16, Alignment::Center, Insets { 10, 0, 10, 0 });
    static Vector<SaverChoice*> s_choices;
    s_choices.clear();
    auto* all = &s_choices;
    for (auto name : Desktop::ScreenSaver::names()) {
        auto& choice = choices.add<SaverChoice>(name, name == chosen);
        all->append(&choice);
        choice.on_action = [all, &choice] {
            for (auto* each : *all)
                each->set_chosen(each == &choice);
            Desktop::Preferences saved("com.vulture.screensaver"_sv);
            saved.set("name"_sv, MUST(JSON::Value::string(choice.name())));
            saved.save();
        };
    }
    auto& filler = choices.add<View>();
    filler.set_stretch(1, 0);
    page.finish();
}

void build_login_items(Page& page)
{
    auto& group = page.add_group("Open at Login"_sv);
    auto const items = Desktop::login_items();
    auto const applications = Desktop::installed_applications();
    auto name_of = [&](StringView path) -> String {
        for (auto const& application : applications) {
            if (application.bundle_path.view() == path)
                return application.name;
        }
        return application_name_from_path(path);
    };
    if (items.is_empty())
        (void)group.rows().add<Row>("Items will open automatically when you log in."_sv);
    for (auto const& item : items) {
        auto const name = name_of(item.view());
        auto& row = group.rows().add<Row>(name.view());
        auto& remove = row.add<Button>(StringView {}, ButtonStyle::Circular);
        remove.set_symbol(Symbol::Minus);
        StringBuilder label;
        label.appendff("Remove {}", name.view());
        remove.set_accessibility_label(label.view());
        auto path = item;
        remove.on_action = [path] {
            Desktop::set_login_item(path.view(), false);
            refresh_soon();
        };
    }
    auto& buttons = page.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
    auto& filler = buttons.add<View>();
    filler.set_stretch(1, 0);
    auto& add = buttons.add<PopUpButton>();
    add.set_accessibility_label("Add Login Item"_sv);
    add.add_item("Add\xE2\x80\xA6"_sv);
    Vector<String> paths;
    for (auto const& application : applications) {
        bool already = false;
        for (auto const& item : items)
            already = already || item.view() == application.bundle_path.view();
        if (already)
            continue;
        add.add_item(application.name.view());
        paths.append(application.bundle_path);
    }
    add.set_selected_index(0);
    add.on_change = [paths = move(paths)](int index) {
        if (index <= 0 || static_cast<size_t>(index - 1) >= paths.size())
            return;
        Desktop::set_login_item(paths[static_cast<size_t>(index - 1)].view(), true);
        refresh_soon();
    };
    page.finish();
}

// How the displays sit next to each other: a little picture of them. Drag one around the main one, and it snaps to its edge.
class ArrangementView final : public View {
public:
    ArrangementView()
    {
        refresh();
    }

    void refresh()
    {
        m_displays = Application::the().connection().displays();
        set_needs_display();
    }

    [[nodiscard]] FloatSize intrinsic_size() const override { return { -1, 180 }; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        for (size_t i = 0; i < m_displays.size(); ++i) {
            auto const rect = place_of(i);
            graphics.fill_rounded(rect, 4, i == 0 ? t.color("accent"_sv) : t.color("disks.free"_sv));
            if (i == 0)
                graphics.fill({ rect.x, rect.y, rect.width, 6 }, Color { 255, 255, 255, 200 });
            StringBuilder label;
            label.appendff("{}", i + 1);
            graphics.draw_text_centered(label.view(), t.named_font("headline"_sv), rect, i == 0 ? Color { 255, 255, 255, 255 } : t.color("label"_sv));
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        for (size_t i = m_displays.size(); i > 0; --i) {
            if (i - 1 != 0 && place_of(i - 1).contains(event.position)) {
                m_dragging = static_cast<int>(i - 1);
                m_drag_from = event.position;
                m_drag_offset = {};
                return true;
            }
        }
        return false;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (m_dragging < 0)
            return;
        m_drag_offset = { event.position.x - m_drag_from.x, event.position.y - m_drag_from.y };
        set_needs_display();
    }

    void mouse_up(MouseEvent const&) override
    {
        if (m_dragging < 0)
            return;
        auto const moved = place_of(static_cast<size_t>(m_dragging));
        auto const main = place_of(0);
        auto const& display = m_displays[static_cast<size_t>(m_dragging)];
        auto const& first = m_displays[0];
        // Whichever side of the main one it's nearest, up against it.
        float const across = moved.x + moved.width / 2 - (main.x + main.width / 2);
        float const down = moved.y + moved.height / 2 - (main.y + main.height / 2);
        int x = display.x;
        int y = display.y;
        if (Base::Math::absolute(across) >= Base::Math::absolute(down)) {
            x = across >= 0 ? first.width : -display.width;
            y = 0;
        } else {
            y = down >= 0 ? first.height : -display.height;
            x = 0;
        }
        int const output = display.output;
        int const width = display.width;
        int const height = display.height;
        m_dragging = -1;
        (void)Application::the().connection().server().set_display(output, x, y, width, height);
        save_layout();
    }

    static void save_layout()
    {
        StringBuilder layout;
        for (auto const& display : Application::the().connection().displays())
            layout.appendff("{} {} {} {} {};", display.output, display.x, display.y, display.width, display.height);
        Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
        preferences.set("displayLayout"_sv, MUST(JSON::Value::string(layout.view())));
        preferences.save();
    }

private:
    [[nodiscard]] FloatRect place_of(size_t index) const
    {
        // All of them fitted in, in the middle.
        int left = 0, top = 0, right = 0, bottom = 0;
        for (auto const& display : m_displays) {
            left = min(left, display.x);
            top = min(top, display.y);
            right = max(right, display.x + display.width);
            bottom = max(bottom, display.y + display.height);
        }
        float const fit = min((width() - 40) / static_cast<float>(max(right - left, 1)), (height() - 30) / static_cast<float>(max(bottom - top, 1)));
        float const start_x = (width() - static_cast<float>(right - left) * fit) / 2;
        float const start_y = (height() - static_cast<float>(bottom - top) * fit) / 2;
        auto const& display = m_displays[index];
        FloatRect rect { start_x + static_cast<float>(display.x - left) * fit + 1, start_y + static_cast<float>(display.y - top) * fit + 1, static_cast<float>(display.width) * fit - 2, static_cast<float>(display.height) * fit - 2 };
        if (static_cast<int>(index) == m_dragging) {
            rect.x += m_drag_offset.x;
            rect.y += m_drag_offset.y;
        }
        return rect;
    }

    Vector<WS::DisplayRecord> m_displays;
    int m_dragging { -1 };
    FloatPoint m_drag_from;
    FloatPoint m_drag_offset;
};

void build_displays(Page& page)
{
    auto& connection = Application::the().connection();
    auto displays = connection.displays();
    auto answer = connection.server().displays();
    bool const mirrored = !answer.is_error() && answer.value().mirrored;
    int const outputs = answer.is_error() ? 1 : answer.value().outputs;
    Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
    ArrangementView* picture = nullptr;
    if (outputs > 1) {
        auto& arrangement = page.add_group("Arrangement"_sv);
        picture = &arrangement.rows().add<ArrangementView>();
        auto& mirror_row = arrangement.rows().add<Row>("Mirror displays"_sv);
        auto& mirror = mirror_row.add<Switch>(mirrored);
        mirror.set_accessibility_label("Mirror displays"_sv);
        mirror.on_action = [&mirror, picture] {
            (void)Application::the().connection().server().set_mirroring(mirror.is_on());
            Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
            saved.set("mirrorDisplays"_sv, JSON::Value(mirror.is_on()));
            saved.save();
            picture->refresh();
        };
    }
    // What each can look like: its usual size, and more or less on the same panel. Four, as a Mac gives.
    auto const& main = displays[0];
    int const natural_width = main.panel_width >= 2000 ? main.panel_width / 2 : main.panel_width;
    int const natural_height = main.panel_width >= 2000 ? main.panel_height / 2 : main.panel_height;
    auto& size_group = page.add_group("Resolution"_sv);
    auto& size_row = size_group.rows().add<Row>("Looks like"_sv);
    auto& sizes = size_row.add<PopUpButton>();
    sizes.set_accessibility_label("Looks like"_sv);
    struct Choice {
        int width;
        int height;
    };
    static Vector<Choice> s_choices;
    s_choices.clear();
    int chosen = 0;
    int const percents[] = { 80, 100, 112, 131 };
    for (int percent : percents) {
        Choice const choice { natural_width * percent / 100, natural_height * percent / 100 };
        StringBuilder label;
        label.appendff("{} \xC3\x97 {}", choice.width, choice.height);
        if (percent == 100)
            label.append(" (Default)"_sv);
        sizes.add_item(label.view());
        if (choice.width == main.width && choice.height == main.height)
            chosen = static_cast<int>(s_choices.size());
        s_choices.append(choice);
    }
    sizes.set_selected_index(chosen);
    sizes.on_change = [](int index) {
        auto& the_connection = Application::the().connection();
        auto const now = the_connection.displays();
        auto const& choice = s_choices[static_cast<size_t>(index)];
        // Those to the right of it move along with its edge.
        int const change = choice.width - now[0].width;
        (void)the_connection.server().set_display(now[0].output, 0, 0, choice.width, choice.height);
        for (size_t i = 1; i < now.size(); ++i) {
            if (now[i].x > 0)
                (void)the_connection.server().set_display(now[i].output, now[i].x + change, now[i].y, now[i].width, now[i].height);
        }
        ArrangementView::save_layout();
    };

    auto& night = page.add_group("Night Shift"_sv);
    int const saved_warmth = static_cast<int>(preferences.number("nightShift"_sv).value_or(0));
    auto& night_row = night.rows().add<Row>("Night Shift"_sv);
    auto& night_switch = night_row.add<Switch>(saved_warmth > 0);
    night_switch.set_accessibility_label("Night Shift"_sv);
    auto& warmth_row = night.rows().add<Row>("Color temperature"_sv);
    auto& warmth = warmth_row.add<Slider>(10.0f, 100.0f, saved_warmth > 0 ? static_cast<float>(saved_warmth) : 60.0f);
    warmth.set_fixed_width(200);
    warmth.set_accessibility_label("Color temperature"_sv);
    auto apply = [&night_switch, &warmth] {
        int const percent = night_switch.is_on() ? static_cast<int>(warmth.value()) : 0;
        (void)Application::the().connection().server().set_warmth(percent);
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("nightShift"_sv, JSON::Value(static_cast<double>(percent)));
        saved.save();
    };
    night_switch.on_action = apply;
    warmth.on_action = apply;
    page.finish();
}

void build_mouse(Page& page)
{
    Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
    auto& group = page.add_group();
    auto& speed_row = group.rows().add<Row>("Tracking speed"_sv);
    auto& speed = speed_row.add<Slider>(25.0f, 400.0f, static_cast<float>(preferences.number("pointerSpeed"_sv).value_or(100)));
    speed.set_fixed_width(200);
    speed.set_accessibility_label("Tracking speed"_sv);
    speed.on_action = [&speed] {
        (void)Application::the().connection().server().set_pointer_speed(static_cast<i32>(speed.value()));
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("pointerSpeed"_sv, JSON::Value(static_cast<double>(speed.value())));
        saved.save();
    };
    auto& natural_row = group.rows().add<Row>("Natural scrolling"_sv);
    auto& natural = natural_row.add<Switch>(preferences.boolean("naturalScrolling"_sv).value_or(true));
    natural.set_accessibility_label("Natural scrolling"_sv);
    natural.on_action = [&natural] {
        (void)Application::the().connection().server().set_natural_scrolling(natural.is_on());
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("naturalScrolling"_sv, JSON::Value(natural.is_on()));
        saved.save();
    };
    page.finish();
}

void build_trackpad(Page& page)
{
    Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
    auto& group = page.add_group();
    auto& tap_row = group.rows().add<Row>("Tap to click"_sv);
    auto& tap = tap_row.add<Switch>(preferences.boolean("tapToClick"_sv).value_or(true));
    tap.set_accessibility_label("Tap to click"_sv);
    tap.on_action = [&tap] {
        (void)Application::the().connection().server().set_tap_to_click(tap.is_on());
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("tapToClick"_sv, JSON::Value(tap.is_on()));
        saved.save();
    };
    auto& natural_row = group.rows().add<Row>("Natural scrolling"_sv);
    auto& natural = natural_row.add<Switch>(preferences.boolean("naturalScrolling"_sv).value_or(true));
    natural.set_accessibility_label("Natural scrolling"_sv);
    natural.on_action = [&natural] {
        (void)Application::the().connection().server().set_natural_scrolling(natural.is_on());
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("naturalScrolling"_sv, JSON::Value(natural.is_on()));
        saved.save();
    };
    auto& gestures = page.add_group("Gestures"_sv);
    (void)gestures.rows().add<Row>("Scroll"_sv, "Two fingers"_sv);
    (void)gestures.rows().add<Row>("Secondary click"_sv, "Tap with two fingers"_sv);
    (void)gestures.rows().add<Row>("Swipe between Spaces"_sv, "Three fingers, left or right"_sv);
    (void)gestures.rows().add<Row>("Overview"_sv, "Three fingers up"_sv);
    page.finish();
}

void build_accessibility(Page& page)
{
    auto& group = page.add_group();
    auto& row = group.rows().add<Row>("Reduce motion"_sv);
    Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
    auto& toggle = row.add<Switch>(preferences.boolean("reduceMotion"_sv).value_or(false));
    toggle.set_accessibility_label("Reduce motion"_sv);
    toggle.on_action = [&toggle] {
        bool const on = toggle.is_on();
        (void)Application::the().connection().server().set_reduce_motion(on);
        Desktop::Preferences saved(StringView(s_preferences, strlen(s_preferences)));
        saved.set("reduceMotion"_sv, JSON::Value(on));
        saved.save();
    };
    page.finish();
}

// ---- Notifications: as Sequoia's pane has it. Kept in com.vulture.notifications, which notificationd, the menu bar and
// the Dock read again when they hear it's changed.

constexpr char const* notifications_changed = "com.vulture.notifications.preferences";

struct ApplicationRules {
    bool allow { true };
    // "none", "temporary" or "persistent".
    String style;
    bool lock_screen { true };
    bool center { true };
    bool badge { true };
    bool sound { true };
    // "default", "always", "whenUnlocked" or "never".
    String previews;
    // "automatic", "byApplication" or "off".
    String grouping;
};

ApplicationRules rules_of(StringView application)
{
    ApplicationRules rules;
    rules.style = MUST(String::from_view("temporary"_sv));
    rules.previews = MUST(String::from_view("default"_sv));
    rules.grouping = MUST(String::from_view("automatic"_sv));
    Desktop::Preferences preferences("com.vulture.notifications"_sv);
    auto const* all = preferences.object("applications"_sv);
    auto const* mine = all ? all->get_object(application) : nullptr;
    if (!mine)
        return rules;
    rules.allow = mine->get_boolean("allow"_sv).value_or(true);
    rules.style = MUST(String::from_view(mine->get_string("style"_sv).value_or("temporary"_sv)));
    rules.lock_screen = mine->get_boolean("lockScreen"_sv).value_or(true);
    rules.center = mine->get_boolean("center"_sv).value_or(true);
    rules.badge = mine->get_boolean("badge"_sv).value_or(true);
    rules.sound = mine->get_boolean("sound"_sv).value_or(true);
    rules.previews = MUST(String::from_view(mine->get_string("previews"_sv).value_or("default"_sv)));
    rules.grouping = MUST(String::from_view(mine->get_string("grouping"_sv).value_or("automatic"_sv)));
    return rules;
}

void save_rules(StringView application, ApplicationRules const& rules)
{
    Desktop::Preferences preferences("com.vulture.notifications"_sv);
    // The other applications' as they were.
    auto all = JSON::Value::object();
    if (auto const* existing = preferences.object("applications"_sv)) {
        auto text = existing->serialize();
        if (!text.is_error()) {
            auto parsed = JSON::parse(text.value().view());
            if (!parsed.is_error() && parsed.value().is_object())
                all = parsed.release_value();
        }
    }
    auto mine = JSON::Value::object();
    MUST(mine.set("allow"_sv, JSON::Value(rules.allow)));
    MUST(mine.set("style"_sv, MUST(JSON::Value::string(rules.style.view()))));
    MUST(mine.set("lockScreen"_sv, JSON::Value(rules.lock_screen)));
    MUST(mine.set("center"_sv, JSON::Value(rules.center)));
    MUST(mine.set("badge"_sv, JSON::Value(rules.badge)));
    MUST(mine.set("sound"_sv, JSON::Value(rules.sound)));
    MUST(mine.set("previews"_sv, MUST(JSON::Value::string(rules.previews.view()))));
    MUST(mine.set("grouping"_sv, MUST(JSON::Value::string(rules.grouping.view()))));
    MUST(all.set(application, move(mine)));
    preferences.set("applications"_sv, move(all));
    preferences.save();
    notify_post(notifications_changed);
}

// What an application's notifications do, in a few words, as the list says under its name.
String rules_summary(ApplicationRules const& rules)
{
    if (!rules.allow)
        return MUST(String::from_view("Off"_sv));
    StringBuilder builder;
    auto add = [&](StringView part) {
        if (builder.length() != 0)
            builder.append(", "_sv);
        builder.append(part);
    };
    if (rules.badge)
        add("Badges"_sv);
    if (rules.sound)
        add("Sounds"_sv);
    if (rules.style.view() == "temporary"_sv)
        add("Banners"_sv);
    else if (rules.style.view() == "persistent"_sv)
        add("Alerts"_sv);
    if (builder.length() == 0)
        builder.append("Notification Center"_sv);
    return MUST(builder.to_string());
}

// Every application that has posted, as notificationd remembers them, in the order of their names.
Vector<String> notifying_applications()
{
    Vector<String> names;
    auto port = IPC::look_up("com.vulture.notificationd"_sv);
    if (port.is_error())
        return names;
    NotificationServiceProxy center(port.release_value());
    auto answer = center.applications();
    if (answer.is_error())
        return names;
    auto json = JSON::parse(answer.value().view());
    if (json.is_error() || !json.value().is_array())
        return names;
    for (size_t i = 0; i < json.value().size(); ++i) {
        if (json.value().at(i).is_string())
            names.append(MUST(String::from_view(json.value().at(i).as_string())));
    }
    for (size_t i = 0; i < names.size(); ++i) {
        for (size_t j = i + 1; j < names.size(); ++j) {
            if (names[j].view() < names[i].view())
                swap(names[i], names[j]);
        }
    }
    return names;
}

StringView icon_of_application(StringView name)
{
    static Vector<Desktop::AppInfo> s_applications = Desktop::installed_applications();
    for (auto const& info : s_applications) {
        if (info.name.view() == name)
            return info.icon.view();
    }
    return "GenericApplication"_sv;
}

// One application in the list: its icon, its name, what its notifications do, and a chevron. Chosen, its own page opens.
class ApplicationRow final : public View {
public:
    explicit ApplicationRow(StringView name)
        : m_name(MUST(String::from_view(name)))
    {
        set_fixed_height(tokens().metric("settings.applicationRowHeight"_sv));
    }

    Function<void()> on_choose;

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const icon = t.metric("settings.applicationIcon"_sv);
        if (auto picture = Desktop::icon_named(icon_of_application(m_name.view()), static_cast<int>(icon), static_cast<int>(graphics.scale())))
            graphics.painter().draw_bitmap(*picture, FloatRect { 0, (height() - icon) / 2, icon, icon });
        float const x = icon + 10;
        auto const summary = rules_summary(rules_of(m_name.view()));
        graphics.draw_text(m_name.view(), t.named_font("body"_sv), { x, height() / 2 - 2 }, t.color("label"_sv));
        graphics.draw_text_truncated(summary.view(), t.named_font("small"_sv), { x, height() / 2 + 13 }, width() - x - 24, t.color("secondaryLabel"_sv));
        graphics.draw_symbol(Symbol::ChevronRight, { width() - 12, (height() - 12) / 2, 8, 12 }, t.color("tertiaryLabel"_sv), 1.6f);
    }

    void mouse_up(MouseEvent const& event) override
    {
        if (FloatRect { 0, 0, width(), height() }.contains(event.position) && on_choose)
            on_choose();
    }
    bool mouse_down(MouseEvent const&) override { return true; }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::Row; }
    String accessibility_label() const override { return m_name; }
    String accessibility_value() const override { return rules_summary(rules_of(m_name.view())); }
    bool accessibility_press() override
    {
        if (on_choose)
            on_choose();
        return true;
    }

private:
    String m_name;
};

// None, Temporary or Persistent, each as a picture of a screen with what it shows in the corner.
class AlertStylePicker final : public Control {
public:
    explicit AlertStylePicker(int chosen)
        : m_chosen(chosen)
    {
    }

    Function<void(int)> on_change;

    void set_chosen(int chosen)
    {
        m_chosen = chosen;
        set_needs_display();
    }

    FloatSize intrinsic_size() const override
    {
        auto const& t = tokens();
        return { (t.metric("settings.alertPictureWidth"_sv) + t.metric("settings.alertPictureSpacing"_sv)) * 3, t.metric("settings.alertPictureHeight"_sv) + 22 };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        static constexpr StringView names[] { "None"_sv, "Temporary"_sv, "Persistent"_sv };
        for (int i = 0; i < 3; ++i) {
            auto const picture = picture_rect(i);
            graphics.fill_rounded(picture, 6, t.color("settings.alertScreen"_sv));
            if (i > 0) {
                // The notification, at the top right.
                FloatRect const card { picture.x + picture.width * 0.45f, picture.y + 6, picture.width * 0.5f, 12 };
                graphics.fill_rounded(card, 3, t.color("settings.alertCard"_sv));
                if (i == 2)
                    graphics.fill({ card.x + card.width * 0.72f, card.y + 2, graphics.pixel(), card.height - 4 }, t.color("settings.alertCardLine"_sv));
            }
            if (i == m_chosen)
                graphics.stroke_rounded({ picture.x - 3, picture.y - 3, picture.width + 6, picture.height + 6 }, 8, 3, t.color("accent"_sv));
            graphics.draw_text_centered(names[i], t.named_font("small"_sv), { picture.x - 8, picture.y + picture.height + 4, picture.width + 16, 16 }, t.color("label"_sv));
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        for (int i = 0; i < 3; ++i) {
            auto const picture = picture_rect(i);
            if (FloatRect { picture.x - 4, picture.y - 4, picture.width + 8, picture.height + 24 }.contains(event.position)) {
                choose(i);
                return true;
            }
        }
        return true;
    }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    String accessibility_label() const override { return MUST(String::from_view("Alert style"_sv)); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        static constexpr StringView names[] { "None"_sv, "Temporary"_sv, "Persistent"_sv };
        for (int i = 0; i < 3; ++i)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::RadioButton, names[i], picture_rect(i), i == m_chosen ? "on"_sv : "off"_sv, is_enabled()));
    }

private:
    [[nodiscard]] FloatRect picture_rect(int index) const
    {
        auto const& t = tokens();
        float const width = t.metric("settings.alertPictureWidth"_sv);
        float const spacing = t.metric("settings.alertPictureSpacing"_sv);
        return { spacing / 2 + (width + spacing) * static_cast<float>(index), 4, width, t.metric("settings.alertPictureHeight"_sv) };
    }

    void choose(int index)
    {
        if (!is_enabled() || index == m_chosen)
            return;
        m_chosen = index;
        set_needs_display();
        if (on_change)
            on_change(index);
    }

    int m_chosen { 1 };
};

// Opening a page under the pane that's showing, with Back in the toolbar: an application's notifications.

void build_application_notifications(Page& page, String const& application)
{
    auto rules = adopt_own(*new ApplicationRules(rules_of(application.view())));
    auto* shared = rules.leak_ptr();
    auto const name = MUST(String::from_view(application.view()));
    auto save = [shared, name] { save_rules(name.view(), *shared); };

    auto& group = page.add_group();
    auto& allow_row = group.rows().add<Row>("Allow notifications"_sv);
    auto& allow = allow_row.add<Switch>(shared->allow);
    allow.set_accessibility_label("Allow notifications"_sv);

    StringBuilder style_title;
    style_title.appendff("{} alert style", application.view());
    auto& style_row = group.rows().add<Row>(style_title.view());
    style_row.set_fixed_height(tokens().metric("settings.alertRowHeight"_sv));
    int const chosen = shared->style.view() == "none"_sv ? 0 : (shared->style.view() == "persistent"_sv ? 2 : 1);
    auto& style = style_row.add<AlertStylePicker>(chosen);
    style.on_change = [shared, save](int index) {
        static constexpr StringView styles[] { "none"_sv, "temporary"_sv, "persistent"_sv };
        shared->style = MUST(String::from_view(styles[index]));
        save();
    };

    Vector<Control*> dependents;
    dependents.append(&style);
    auto add_switch = [&](StringView title, bool ApplicationRules::* field) {
        auto& row = group.rows().add<Row>(title);
        auto& toggle = row.add<Switch>(shared->*field);
        toggle.set_accessibility_label(title);
        toggle.on_action = [shared, save, field, &toggle] {
            shared->*field = toggle.is_on();
            save();
        };
        dependents.append(&toggle);
    };
    add_switch("Show notifications on lock screen"_sv, &ApplicationRules::lock_screen);
    add_switch("Show in Notification Center"_sv, &ApplicationRules::center);
    add_switch("Badge application icon"_sv, &ApplicationRules::badge);
    add_switch("Play sound for notification"_sv, &ApplicationRules::sound);

    auto& more = page.add_group();
    auto& previews_row = more.rows().add<Row>("Show previews"_sv);
    auto& previews = previews_row.add<PopUpButton>();
    previews.set_accessibility_label("Show previews"_sv);
    previews.add_item("Default (Always)"_sv);
    previews.add_item("Always"_sv);
    previews.add_item("When Unlocked"_sv);
    previews.add_item("Never"_sv);
    static constexpr StringView preview_values[] { "default"_sv, "always"_sv, "whenUnlocked"_sv, "never"_sv };
    for (int i = 0; i < 4; ++i) {
        if (shared->previews.view() == preview_values[i])
            previews.set_selected_index(i);
    }
    previews.on_change = [shared, save](int index) {
        shared->previews = MUST(String::from_view(preview_values[index]));
        save();
    };
    auto& grouping_row = more.rows().add<Row>("Notification grouping"_sv);
    auto& grouping = grouping_row.add<PopUpButton>();
    grouping.set_accessibility_label("Notification grouping"_sv);
    grouping.add_item("Automatic"_sv);
    grouping.add_item("By Application"_sv);
    grouping.add_item("Off"_sv);
    static constexpr StringView grouping_values[] { "automatic"_sv, "byApplication"_sv, "off"_sv };
    for (int i = 0; i < 3; ++i) {
        if (shared->grouping.view() == grouping_values[i])
            grouping.set_selected_index(i);
    }
    grouping.on_change = [shared, save](int index) {
        shared->grouping = MUST(String::from_view(grouping_values[index]));
        save();
    };
    dependents.append(&previews);
    dependents.append(&grouping);

    // With notifications off, the rest have nothing to say.
    auto* held = adopt_own(*new Vector<Control*>(move(dependents))).leak_ptr();
    for (auto* control : *held)
        control->set_enabled(shared->allow);
    allow.on_action = [shared, save, held, &allow] {
        shared->allow = allow.is_on();
        for (auto* control : *held)
            control->set_enabled(shared->allow);
        save();
    };
    page.finish();
}

void build_notifications(Page& page)
{
    Desktop::Preferences preferences("com.vulture.notifications"_sv);
    auto saved = [](StringView key, JSON::Value value) {
        Desktop::Preferences again("com.vulture.notifications"_sv);
        again.set(key, move(value));
        again.save();
        notify_post(notifications_changed);
    };
    auto& center = page.add_group("Notification Center"_sv);
    auto& previews_row = center.rows().add<Row>("Show previews"_sv);
    auto& previews = previews_row.add<PopUpButton>();
    previews.set_accessibility_label("Show previews"_sv);
    previews.add_item("Always"_sv);
    previews.add_item("When Unlocked"_sv);
    previews.add_item("Never"_sv);
    static constexpr StringView preview_values[] { "always"_sv, "whenUnlocked"_sv, "never"_sv };
    auto const previews_now = preferences.string("previews"_sv).value_or("always"_sv);
    for (int i = 0; i < 3; ++i) {
        if (previews_now == preview_values[i])
            previews.set_selected_index(i);
    }
    previews.on_change = [saved](int index) { saved("previews"_sv, MUST(JSON::Value::string(preview_values[index]))); };
    auto add_switch = [&](StringView title, StringView key) {
        auto& row = center.rows().add<Row>(title);
        auto& toggle = row.add<Switch>(preferences.boolean(key).value_or(key == "whenMirroring"_sv ? false : true));
        toggle.set_accessibility_label(title);
        auto const key_string = MUST(String::from_view(key));
        toggle.on_action = [saved, key_string, &toggle] { saved(key_string.view(), JSON::Value(toggle.is_on())); };
    };
    add_switch("Allow notifications when the display is sleeping"_sv, "whenSleeping"_sv);
    add_switch("Allow notifications when the screen is locked"_sv, "whenLocked"_sv);
    add_switch("Allow notifications when mirroring or sharing the display"_sv, "whenMirroring"_sv);

    auto names = notifying_applications();
    auto& applications = page.add_group("Application Notifications"_sv);
    if (names.is_empty())
        (void)applications.rows().add<Row>("No applications have sent notifications"_sv);
    for (auto const& name : names) {
        auto& row = applications.rows().add<ApplicationRow>(name.view());
        auto const captured = MUST(String::from_view(name.view()));
        row.on_choose = [captured] {
            Function<void(Page&)> build = [captured](Page& detail) { build_application_notifications(detail, captured); };
            if (s_push_page)
                s_push_page(captured.view(), build);
        };
    }
    page.finish();
}

String read_uname_line()
{
    struct utsname name;
    StringBuilder builder;
    if (uname(&name) == 0)
        builder.appendff("{} {}", name.sysname, name.release);
    return MUST(builder.to_string());
}

// How full each volume is.
void build_storage(Page& page)
{
    size_t length = 0;
    Vector<vulture_mount_info> mounts;
    if (sysctlbyname("vfs.mounts", nullptr, &length, nullptr, 0) == 0) {
        length += 8 * sizeof(vulture_mount_info);
        MUST(mounts.try_resize(length / sizeof(vulture_mount_info)));
        if (sysctlbyname("vfs.mounts", mounts.data(), &length, nullptr, 0) != 0)
            length = 0;
        MUST(mounts.try_resize(length / sizeof(vulture_mount_info)));
    }
    auto const add_volume = [&](StringView name, vulture_mount_info const& mount) {
        u64 const capacity = mount.blocks * mount.block_size;
        u64 const available = mount.free_blocks * mount.block_size;
        auto& group = page.add_group(name);
        StringBuilder used;
        used.appendff("{} used of {}", DiskManagement::size_text(capacity - available).view(), DiskManagement::size_text(capacity).view());
        (void)group.rows().add<Row>("Used"_sv, used.view());
        (void)group.rows().add<Row>("Available"_sv, DiskManagement::size_text(available).view());
    };
    for (auto const& mount : mounts) {
        StringView const point { mount.point, strlen(mount.point) };
        if (point == "/System/Volumes/Data"_sv)
            add_volume("Vulture HD"_sv, mount);
        else if (point.starts_with("/Volumes/"_sv) && point != "/Volumes/EFI"_sv)
            add_volume(point.substring_view(9), mount);
    }
    page.finish();
}

// Which of the systems on the machine it starts from.
void build_startup_disk(Page& page)
{
    auto& group = page.add_group("Start up from"_sv);
    auto const disks = DiskManagement::list_disks();
    size_t found = 0;
    if (!disks.is_error()) {
        for (auto const& disk : disks.value()) {
            if (disk.content() != "vulture-system"_sv)
                continue;
            ++found;
            StringBuilder title;
            title.appendff("{} ({})", disk.label(), disk.name());
            auto& row = group.rows().add<Row>(title.view());
            auto& button = row.add<Button>("Choose"_sv);
            StringBuilder line;
            line.appendff("set-startup {}", disk.name());
            button.on_action = [line = MUST(line.to_string())] {
                (void)DiskManagement::request(line.view());
            };
        }
    }
    if (found == 0)
        (void)group.rows().add<Row>("This system is running from memory: there's nothing on a disk to start from."_sv);
    page.finish();
}

void build_about(Page& page)
{
    auto& group = page.add_group();
    (void)group.rows().add<Row>("Name"_sv, "vultureOS"_sv);
    auto version = read_uname_line();
    (void)group.rows().add<Row>("System"_sv, version.view());
    struct utsname name;
    if (uname(&name) == 0)
        (void)group.rows().add<Row>("Machine"_sv, StringView(name.machine, strlen(name.machine)));
    StringBuilder processors;
    processors.appendff("{}", sysconf(_SC_NPROCESSORS_ONLN));
    (void)group.rows().add<Row>("Processors"_sv, processors.view());
    auto screen = Platform::the().screen_size();
    StringBuilder display;
    display.appendff("{} by {} points, at {}x", screen.width, screen.height, Platform::the().scale());
    (void)group.rows().add<Row>("Display"_sv, display.view());
    page.finish();
}

class Settings {
public:
    explicit Settings(StringView first_pane)
    {
        auto const& t = tokens();
        m_window = adopt_own(*new UI::Window("Settings"_sv, { 200, 90, 880, 600 }, WS::WindowTitled | WS::WindowUnifiedTitle));
        m_window->set_transparent(true);
        auto& root = m_window->set_content<Background>();
        auto& split = root.add<SplitView>();
        float sidebar_width = t.metric("settings.sidebarWidth"_sv);
        m_sidebar = &split.add_pane<Sidebar>(sidebar_width, sidebar_width);
        m_sidebar->set_top_padding(t.metric("window.toolbarCompact"_sv));
        auto& right = split.add_pane<StackView>(0, 300, Axis::Vertical);
        m_toolbar = &right.add<Toolbar>();
        m_toolbar->set_left_padding(t.metric("toolbar.paddingRight"_sv));
        // Back and forward, before the title, as Sequoia's has them: back from an application's notifications, and there again.
        m_back = &m_toolbar->add_button(Symbol::ChevronLeft, [this] { go_back(); }, "Back"_sv);
        m_forward = &m_toolbar->add_button(Symbol::ChevronRight, [this] { go_forward(); }, "Forward"_sv);
        m_back->set_enabled(false);
        m_forward->set_enabled(false);
        m_title = &m_toolbar->add_item<Label>("Settings"_sv, t.named_font("headline"_sv));
        s_push_page = [this](StringView title, Function<void(Page&)>& build) { push_page(title, move(build)); };
        s_refresh_page = [this] {
            drop_detail();
            m_back->set_enabled(false);
            if (!m_made_when_shown[m_current])
                make_again(m_current);
            show_page(m_current);
        };
        m_pages = &right.add<View>();
        m_pages->set_stretch(0, 1);
        split.set_frame({ 0, 0, 880, 600 });
        MaterialRegion region { { 0, 0, static_cast<int>(sidebar_width), 600 }, WS::Material::Sidebar };
        m_window->set_materials(Span<MaterialRegion const>(&region, 1));

        add_pane("Appearance"_sv, Symbol::Contrast, build_appearance);
        add_pane("Accessibility"_sv, Symbol::Accessibility, build_accessibility);
        add_pane("Control Center"_sv, Symbol::Toggles, build_control_center);
        add_pane("Wallpaper"_sv, Symbol::Photo, build_wallpaper);
        add_pane("Screen Saver"_sv, Symbol::Display, build_screen_saver);
        add_pane("Desktop & Dock"_sv, Symbol::Sidebar, build_dock);
        add_pane("Displays"_sv, Symbol::Display, build_displays);
        add_pane("Network"_sv, Symbol::Globe, build_network);
        add_pane("Sound"_sv, Symbol::Speaker, build_sound);
        add_pane("Keyboard"_sv, Symbol::Keyboard, build_keyboard);
        add_pane("Mouse"_sv, Symbol::Mouse, build_mouse);
        add_pane("Trackpad"_sv, Symbol::Trackpad, build_trackpad);
        add_pane("Language & Region"_sv, Symbol::Globe, build_language_region);
        add_pane("Notifications"_sv, Symbol::Bell, build_notifications, true);
        add_pane("Focus"_sv, Symbol::Moon, build_focus);
        add_pane("Privacy & Security"_sv, Symbol::Lock, build_privacy);
        add_pane("Lock Screen"_sv, Symbol::Lock, build_lock_screen);
        add_pane("Users & Groups"_sv, Symbol::Person, build_users, true);
        add_pane("Login Items"_sv, Symbol::Applications, build_login_items, true);
        add_pane("Storage"_sv, Symbol::Drive, build_storage);
        add_pane("Startup Disk"_sv, Symbol::Drive, build_startup_disk);
        add_pane("About"_sv, Symbol::Information, build_about);
        m_sidebar->on_select = [this](size_t row) {
            drop_detail();
            m_forward_title = {};
            m_forward_build = nullptr;
            m_forward->set_enabled(false);
            show_page(row);
        };
        size_t first = 0;
        for (size_t i = 0; i < m_names.size(); ++i) {
            if (m_names[i].view() == first_pane)
                first = i;
        }
        m_sidebar->select_row(first);
        show_page(first);
        m_window->on_closed = [] { Application::the().quit(); };
        // Back in front, a pane that says what's changed says it again.
        m_window->on_key_change = [this](bool key) {
            if (!key || m_detail || !m_made_when_shown[m_current])
                return;
            make_again(m_current);
            m_pages->children()[m_current]->set_hidden(false);
            m_pages->set_needs_layout();
        };
        m_window->show();
    }

private:
    class Background final : public View {
    public:
        void draw(Graphics& graphics) override
        {
            auto const& t = tokens();
            float sidebar = t.metric("settings.sidebarWidth"_sv);
            graphics.fill({ sidebar, 0, width() - sidebar, height() }, t.color("windowBackground"_sv));
        }
    };

    class PagesLayout final : public Layout {
    public:
        void apply(View& view) override
        {
            for (auto const& child : view.children())
                child->set_frame({ 0, 0, view.width(), view.height() });
        }
        [[nodiscard]] FloatSize preferred_size(View const&) const override { return { 0, 0 }; }
    };

    // A pane that's made again each time it's shown says what's changed since: which applications have posted, for
    // Notifications.
    void add_pane(StringView name, Symbol symbol, void (*build)(Page&), bool made_when_shown = false)
    {
        m_names.append(MUST(String::from_view(name)));
        m_builds.append(build);
        m_made_when_shown.append(made_when_shown);
        m_sidebar->add_row(name, symbol);
        // Longer than the window, some of them: they scroll, under the title.
        auto& scroll = m_pages->add<ScrollView>();
        scroll.set_scrolls_horizontally(false);
        auto& page = scroll.make_document<Page>();
        build(page);
        scroll.set_hidden(true);
        if (!m_pages->layout_manager())
            m_pages->set_layout<PagesLayout>();
    }

    void make_again(size_t index)
    {
        auto* old = m_pages->children()[index].ptr();
        auto taken = m_pages->take_child(*old);
        auto* gone = taken.leak_ptr();
        Platform::the().defer([gone] { delete gone; });
        auto scroll = adopt_own(*new ScrollView);
        scroll->set_scrolls_horizontally(false);
        auto& page = scroll->make_document<Page>();
        m_builds[index](page);
        m_pages->adopt_child_at(index, move(scroll));
    }

    void push_page(StringView title, Function<void(Page&)> build)
    {
        drop_detail();
        auto& scroll = m_pages->add<ScrollView>();
        scroll.set_scrolls_horizontally(false);
        auto& page = scroll.make_document<Page>();
        build(page);
        m_detail = &scroll;
        m_detail_title = MUST(String::from_view(title));
        m_detail_build = move(build);
        for (auto& child : m_pages->children())
            child->set_hidden(child.ptr() != m_detail);
        m_title->set_text(title);
        m_window->set_title(title);
        m_pages->set_needs_layout();
        m_back->set_enabled(true);
        m_forward->set_enabled(false);
        printf("settings: %s\n", m_detail_title.characters());
        fflush(stdout);
    }

    void drop_detail()
    {
        if (!m_detail)
            return;
        auto owned = m_pages->take_child(*m_detail);
        m_detail = nullptr;
        auto* old = owned.leak_ptr();
        Platform::the().defer([old] { delete old; });
    }

    void go_back()
    {
        if (!m_detail)
            return;
        m_forward_title = move(m_detail_title);
        m_forward_build = move(m_detail_build);
        drop_detail();
        m_back->set_enabled(false);
        m_forward->set_enabled(true);
        show_page(m_current);
    }

    void go_forward()
    {
        if (!m_forward_build)
            return;
        auto title = move(m_forward_title);
        auto build = move(m_forward_build);
        push_page(title.view(), move(build));
    }

    void show_page(size_t index)
    {
        m_current = index;
        if (m_made_when_shown[index])
            make_again(index);
        for (size_t i = 0; i < m_pages->children().size(); ++i)
            m_pages->children()[i]->set_hidden(i != index);
        m_title->set_text(m_names[index].view());
        m_pages->set_needs_layout();
        m_window->set_title(m_names[index].view());
        printf("settings: %s\n", m_names[index].characters());
        fflush(stdout);
    }

    OwnPtr<UI::Window> m_window;
    Sidebar* m_sidebar { nullptr };
    Toolbar* m_toolbar { nullptr };
    Label* m_title { nullptr };
    View* m_pages { nullptr };
    Vector<String> m_names;
    Vector<void (*)(Page&)> m_builds;
    Vector<bool> m_made_when_shown;
    ToolbarButton* m_back { nullptr };
    ToolbarButton* m_forward { nullptr };
    size_t m_current { 0 };
    View* m_detail { nullptr };
    String m_detail_title;
    Function<void(Page&)> m_detail_build;
    String m_forward_title;
    Function<void(Page&)> m_forward_build;
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    StringView first_pane = "Appearance"_sv;
    bool still = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            still = true;
        else
            first_pane = StringView(argv[i], strlen(argv[i]));
    }
    auto application_or_error = Application::create("Settings"_sv, "com.vulture.Settings"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Settings: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    if (still)
        set_motion_enabled(false);
    application->set_main_menu(application->standard_menus());
    Settings settings(first_pane);
    return application->exec();
}
