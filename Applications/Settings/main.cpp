/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/privacy.h>
#include <LibIPC/Bootstrap.h>
#include <LibJSON/JSON.h>
#include <PrivacyEndpoint.h>
#include <Desktop.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <LibCore/Account.h>
#include <LibDiskManagement/Disks.h>
#include <LibLocale/Locale.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Layout.h>
#include <LibUI/Motion.h>
#include <LibUI/ScrollView.h>
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
    WallpaperPicker()
    {
        StringView const names[] = { "Dusk"_sv, "Ridge"_sv, "Ember"_sv, "Glacier"_sv, "Graphite"_sv };
        for (auto name : names)
            m_names.append(MUST(String::from_view(name)));
        Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
        m_selected = MUST(String::from_view(preferences.string("wallpaper"_sv).value_or("Dusk"_sv)));
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
            if (m_names[i].view() == m_selected.view())
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
        m_selected = MUST(String::from_view(m_names[static_cast<size_t>(index)].view()));
        (void)Application::the().connection().server().set_wallpaper(m_selected.view());
        Desktop::Preferences preferences(StringView(s_preferences, strlen(s_preferences)));
        preferences.set("wallpaper"_sv, MUST(JSON::Value::string(m_selected.view())));
        preferences.save();
        set_needs_display();
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
    String m_selected;
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
    auto& group = page.add_group("Wallpaper"_sv);
    auto& picker = group.rows().add<WallpaperPicker>();
    picker.set_fixed_height(picker.intrinsic_size().height + 16);
    page.finish();
}

void build_dock(Page& page)
{
    auto& group = page.add_group("Dock"_sv);
    auto& size_row = group.rows().add<Row>("Size"_sv);
    auto& size = size_row.add<Slider>(32.0f, 96.0f, tokens().metric("dock.iconSize"_sv));
    size.set_fixed_width(220);
    auto& magnification_row = group.rows().add<Row>("Magnification"_sv);
    (void)magnification_row.add<Switch>(true);
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

void build_users(Page& page)
{
    auto& group = page.add_group("Users"_sv);
    setpwent();
    while (auto* entry = getpwent()) {
        if (entry->pw_uid < 500 || entry->pw_uid >= 60000)
            continue;
        StringView name { entry->pw_name, strlen(entry->pw_name) };
        StringView full { entry->pw_gecos ? entry->pw_gecos : "", entry->pw_gecos ? strlen(entry->pw_gecos) : 0 };
        (void)group.rows().add<Row>(full.is_empty() ? name : full, name);
    }
    endpwent();
    page.finish();
}

void build_notifications(Page& page)
{
    auto& group = page.add_group();
    auto& row = group.rows().add<Row>("Test notification"_sv);
    auto& send = row.add<Button>("Send"_sv);
    send.on_action = [] { Desktop::post_notification("Settings"_sv, "It works"_sv, "This is how notifications look."_sv); };
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
        m_title = &m_toolbar->add_item<Label>("Settings"_sv, t.named_font("headline"_sv));
        m_pages = &right.add<View>();
        m_pages->set_stretch(0, 1);
        split.set_frame({ 0, 0, 880, 600 });
        MaterialRegion region { { 0, 0, static_cast<int>(sidebar_width), 600 }, WS::Material::Sidebar };
        m_window->set_materials(Span<MaterialRegion const>(&region, 1));

        add_pane("Appearance"_sv, Symbol::ClearCircle, build_appearance);
        add_pane("Wallpaper"_sv, Symbol::File, build_wallpaper);
        add_pane("Desktop & Dock"_sv, Symbol::Sidebar, build_dock);
        add_pane("Network"_sv, Symbol::Globe, build_network);
        add_pane("Sound"_sv, Symbol::Speaker, build_sound);
        add_pane("Keyboard"_sv, Symbol::Plus, build_keyboard);
        add_pane("Language & Region"_sv, Symbol::Globe, build_language_region);
        add_pane("Notifications"_sv, Symbol::Question, build_notifications);
        add_pane("Privacy & Security"_sv, Symbol::Lock, build_privacy);
        add_pane("Users"_sv, Symbol::Checkmark, build_users);
        add_pane("Storage"_sv, Symbol::Drive, build_storage);
        add_pane("Startup Disk"_sv, Symbol::Drive, build_startup_disk);
        add_pane("About"_sv, Symbol::Folder, build_about);
        m_sidebar->on_select = [this](size_t row) { show_page(row); };
        size_t first = 0;
        for (size_t i = 0; i < m_names.size(); ++i) {
            if (m_names[i].view() == first_pane)
                first = i;
        }
        m_sidebar->select_row(first);
        show_page(first);
        m_window->on_closed = [] { Application::the().quit(); };
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

    void add_pane(StringView name, Symbol symbol, void (*build)(Page&))
    {
        m_names.append(MUST(String::from_view(name)));
        m_sidebar->add_row(name, symbol);
        auto& page = m_pages->add<Page>();
        build(page);
        page.set_hidden(true);
        if (!m_pages->layout_manager())
            m_pages->set_layout<PagesLayout>();
    }

    void show_page(size_t index)
    {
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
