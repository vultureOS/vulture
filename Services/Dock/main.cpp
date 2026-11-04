/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibBase/Math.h>
#include <LibUI/Application.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The Dock: what is kept there and what is running, on a shelf of frosted glass, that
// swells under the pointer. What has a window minimized keeps a tile for it; what is
// dropped on the Trash goes there.

using namespace UI;
using namespace Gfx;
namespace Math = Base::Math;

namespace {

bool s_still;

Appearance const& tokens()
{
    return Appearance::the();
}

struct Tile {
    enum class Kind : u8 {
        Application,
        Separator,
        MinimizedWindow,
        Trash,
    };
    Kind kind { Kind::Application };
    String title;
    String icon;
    String identifier;
    String bundle_path;
    i32 pid { 0 };
    u32 window_id { 0 };
    bool pinned { false };
    // Where it comes out now.
    float center { 0 };
    float size { 0 };
    // Bouncing since (seconds), while it starts.
    double bouncing_since { -1 };
};

// What is kept there to begin with: Files always first.
StringView const s_default_tiles[] = {
    "/System/Library/CoreServices/Files.app"_sv,
    "/System/Applications/Launcher.app"_sv,
    "/System/Applications/Settings.app"_sv,
    "/System/Applications/Terminal.app"_sv,
    "/System/Applications/Text Editor.app"_sv,
    "/System/Applications/Viewer.app"_sv,
    "/System/Applications/Calculator.app"_sv,
    "/System/Applications/Monitor.app"_sv,
    "/System/Applications/Widget Gallery.app"_sv,
};

class DockView final : public View {
public:
    DockView()
    {
        for (auto path : s_default_tiles) {
            auto bundle = Desktop::read_bundle(path);
            if (bundle.is_error())
                continue;
            Tile tile;
            tile.title = MUST(String::from_view(bundle.value().name.view()));
            tile.icon = MUST(String::from_view(bundle.value().icon.view()));
            tile.identifier = MUST(String::from_view(bundle.value().identifier.view()));
            tile.bundle_path = MUST(String::from_view(path));
            tile.pinned = true;
            m_pinned.append(move(tile));
        }
        rebuild({});
    }

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        compute_layout();
        for (auto const& tile : m_tiles) {
            if (tile.kind != Tile::Kind::Separator)
                parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, tile.title.view(), { tile.center - tile.size / 2, m_shelf.y + (m_shelf.height - tile.size) / 2, tile.size, tile.size }));
        }
    }

    void set_state(Desktop::ShellState const& state)
    {
        m_state_applications.clear();
        for (auto const& application : state.applications) {
            if (application.identifier.view() == "com.vulture.Files"_sv)
                m_files_has_windows = !application.windows.is_empty();
            RunningApplication running;
            running.pid = application.pid;
            running.identifier = MUST(String::from_view(application.identifier.view()));
            running.name = MUST(String::from_view(application.name.view()));
            for (auto const& entry : application.windows) {
                if (entry.minimized) {
                    MinimizedEntry minimized;
                    minimized.id = entry.id;
                    minimized.title = MUST(String::from_view(entry.title.view()));
                    minimized.icon = MUST(String::from_view("Document"_sv));
                    m_minimized_pending.append(move(minimized));
                }
            }
            m_state_applications.append(move(running));
        }
        rebuild(state);
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        compute_layout();
        float radius = t.metric("dock.shelfRadius"_sv);
        graphics.stroke_rounded(m_shelf, radius, graphics.pixel(), t.color("dock.border"_sv));
        float dot = t.metric("dock.dotSize"_sv);
        double now = Platform::the().now();
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            auto const& tile = m_tiles[i];
            if (tile.kind == Tile::Kind::Separator) {
                graphics.fill({ tile.center - 0.5f, m_shelf.y + 8, 1, m_shelf.height - 16 }, t.color("dock.separator"_sv));
                continue;
            }
            float bottom = m_shelf.bottom() - t.metric("dock.padding"_sv) - dot;
            float lift = 0;
            if (tile.bouncing_since >= 0) {
                double elapsed = now - tile.bouncing_since;
                lift = static_cast<float>(Math::absolute(Math::sine(elapsed * 6))) * t.metric("dock.bounceHeight"_sv);
            }
            FloatRect place { tile.center - tile.size / 2, bottom - tile.size - lift, tile.size, tile.size };
            auto icon = Desktop::icon_named(tile.icon.view(), 64, static_cast<int>(graphics.scale()));
            if (icon)
                graphics.painter().draw_bitmap(*icon, place);
            bool running = tile.pid != 0 && tile.kind == Tile::Kind::Application;
            if (running)
                graphics.fill_circle({ tile.center, m_shelf.bottom() - dot / 2 - 2 }, dot / 2, t.color("dock.dot"_sv));
        }
        if (m_hovered >= 0 && m_hovered_inside && static_cast<size_t>(m_hovered) < m_tiles.size()) {
            auto const& tile = m_tiles[static_cast<size_t>(m_hovered)];
            auto font = t.named_font("body"_sv);
            float text_width = Appearance::text_width(font, tile.title.view());
            float label_height = t.metric("dock.labelHeight"_sv);
            float label_width = text_width + 20;
            float y = m_shelf.bottom() - t.metric("dock.padding"_sv) - dot - tile.size - t.metric("dock.labelGap"_sv) - label_height;
            FloatRect label { clamp(tile.center - label_width / 2, 4.0f, width() - label_width - 4), y, label_width, label_height };
            graphics.fill_rounded(label, 6, t.color("dock.labelBackground"_sv));
            graphics.draw_text_centered(tile.title.view(), font, label, t.color("dock.labelText"_sv));
        }
    }

    void mouse_moved(MouseEvent const& event) override
    {
        m_pointer_x = event.position.x;
        m_hovered_inside = m_shelf.contains(event.position);
        set_magnifying(m_hovered_inside);
        update_hover();
        update_shape();
        set_needs_display();
    }

    void mouse_exited() override
    {
        m_hovered_inside = false;
        m_hovered = -1;
        set_magnifying(false);
        set_needs_display();
    }

    bool mouse_down(MouseEvent const& event) override
    {
        m_pointer_x = event.position.x;
        update_hover();
        if (m_hovered < 0)
            return false;
        auto const& tile = m_tiles[static_cast<size_t>(m_hovered)];
        if (event.is_right_click()) {
            show_menu(static_cast<size_t>(m_hovered));
            return false;
        }
        m_pressed = m_hovered;
        (void)tile;
        return true;
    }

    [[nodiscard]] bool accepts_drop(StringView type) const override { return type == "public.file-url"_sv; }

    void perform_drop(StringView, ReadonlyBytes data) override
    {
        // Onto the Trash, which is all that takes things here.
        StringView from { reinterpret_cast<char const*>(data.data()), data.size() };
        size_t name_at = from.length();
        while (name_at > 0 && from[name_at - 1] != '/')
            --name_at;
        StringBuilder target;
        target.append(Desktop::home_directory().view());
        target.append("/.Trash/"_sv);
        target.append(from.substring_view(name_at, from.length() - name_at));
        auto source = MUST(String::from_view(from));
        auto destination = MUST(target.to_string());
        (void)rename(source.characters(), destination.characters());
        rebuild({});
    }

    void mouse_up(MouseEvent const& event) override
    {
        update_hover();
        int pressed = m_pressed;
        m_pressed = -1;
        if (pressed < 0 || pressed != m_hovered || !m_shelf.contains(event.position))
            return;
        activate_tile(static_cast<size_t>(pressed));
    }

    void animation_frame()
    {
        bool any_bouncing = false;
        double now = Platform::the().now();
        for (auto& tile : m_tiles) {
            if (tile.bouncing_since >= 0) {
                if (now - tile.bouncing_since > 3)
                    tile.bouncing_since = -1;
                else
                    any_bouncing = true;
            }
        }
        if (any_bouncing)
            set_needs_display();
        else if (m_bounce_timer >= 0) {
            Platform::the().cancel_timer(m_bounce_timer);
            m_bounce_timer = -1;
        }
    }

    void update_shape()
    {
        if (!window())
            return;
        compute_layout();
        auto const& t = tokens();
        IntRect region { static_cast<int>(m_shelf.x), static_cast<int>(m_shelf.y), static_cast<int>(m_shelf.width + 0.999f), static_cast<int>(m_shelf.height + 0.999f) };
        if (region == m_last_shelf)
            return;
        m_last_shelf = region;
        MaterialRegion material { region, WS::Material::UnderWindow, static_cast<int>(t.metric("dock.shelfRadius"_sv)) };
        window()->set_materials(Span<MaterialRegion const>(&material, 1));
        window()->backend().set_input_region(region);
        // Where windows go.
        auto origin = window()->frame().origin();
        (void)Application::the().connection().server().set_minimize_target(origin.x + region.right() - 40, origin.y + region.y + 20);
    }

private:
    struct RunningApplication {
        i32 pid;
        String identifier;
        String name;
    };
    struct MinimizedEntry {
        u32 id;
        String title;
        String icon;
    };

    void rebuild(Desktop::ShellState const& state)
    {
        (void)state;
        double now = Platform::the().now();
        Vector<Tile> old = move(m_tiles);
        m_tiles = {};
        auto bouncing_since = [&](StringView identifier) -> double {
            for (auto const& tile : old) {
                if (tile.identifier.view() == identifier)
                    return tile.bouncing_since;
            }
            return -1;
        };
        auto copied = [](Tile const& tile) {
            Tile duplicate;
            duplicate.kind = tile.kind;
            duplicate.title = MUST(String::from_view(tile.title.view()));
            duplicate.icon = MUST(String::from_view(tile.icon.view()));
            duplicate.identifier = MUST(String::from_view(tile.identifier.view()));
            duplicate.bundle_path = MUST(String::from_view(tile.bundle_path.view()));
            duplicate.pinned = tile.pinned;
            return duplicate;
        };
        (void)now;
        for (auto const& pinned : m_pinned) {
            auto tile = copied(pinned);
            for (auto const& running : m_state_applications) {
                if (running.identifier.view() == tile.identifier.view())
                    tile.pid = running.pid;
            }
            tile.bouncing_since = tile.pid ? -1 : bouncing_since(tile.identifier.view());
            m_tiles.append(move(tile));
        }
        // Whatever else is running.
        for (auto const& running : m_state_applications) {
            bool pinned = false;
            for (auto const& tile : m_pinned) {
                if (tile.identifier.view() == running.identifier.view())
                    pinned = true;
            }
            if (pinned)
                continue;
            Tile tile;
            tile.title = MUST(String::from_view(running.name.view()));
            auto bundle = Desktop::application_with_identifier(running.identifier.view());
            tile.icon = MUST(String::from_view(bundle.has_value() ? bundle.value().icon.view() : "GenericApplication"_sv));
            tile.identifier = MUST(String::from_view(running.identifier.view()));
            tile.pid = running.pid;
            m_tiles.append(move(tile));
        }
        Tile separator;
        separator.kind = Tile::Kind::Separator;
        m_tiles.append(move(separator));
        for (auto& minimized : m_minimized_pending) {
            Tile tile;
            tile.kind = Tile::Kind::MinimizedWindow;
            tile.title = move(minimized.title);
            tile.icon = move(minimized.icon);
            tile.window_id = minimized.id;
            m_tiles.append(move(tile));
        }
        m_minimized_pending.clear();
        Tile trash;
        trash.kind = Tile::Kind::Trash;
        trash.title = MUST(String::from_view("Trash"_sv));
        trash.icon = MUST(String::from_view(trash_has_something() ? "TrashFull"_sv : "Trash"_sv));
        m_tiles.append(move(trash));
        update_shape();
        set_needs_display();
    }

    [[nodiscard]] static bool trash_has_something()
    {
        StringBuilder path;
        path.append(Desktop::home_directory().view());
        path.append("/.Trash"_sv);
        auto terminated = MUST(path.to_string());
        DIR* directory = opendir(terminated.characters());
        if (!directory)
            return false;
        bool found = false;
        while (auto* entry = readdir(directory)) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
                found = true;
        }
        closedir(directory);
        return found;
    }

    void set_magnifying(bool magnifying)
    {
        if (magnifying == m_magnifying_wanted)
            return;
        m_magnifying_wanted = magnifying;
        float from = m_magnify;
        float to = magnifying ? 1 : 0;
        if (m_magnify_animation)
            motion().cancel(m_magnify_animation);
        m_magnify_animation = animate(Animation::Timing::ease_out(magnifying ? 0.12f : 0.2f), [this, from, to](float progress) {
            m_magnify = from + (to - from) * progress;
            update_shape();
            set_needs_display();
        }, [this] { m_magnify_animation = 0; });
    }

    void compute_layout()
    {
        auto const& t = tokens();
        float base = t.metric("dock.iconSize"_sv);
        float magnified = t.metric("dock.magnifiedSize"_sv);
        float spacing = t.metric("dock.spacing"_sv);
        float padding = t.metric("dock.padding"_sv);
        float separator = t.metric("dock.separator"_sv);
        float dot = t.metric("dock.dotSize"_sv);

        // First at rest, to know where each is, then swelled around the pointer.
        float total = 0;
        for (auto const& tile : m_tiles)
            total += (tile.kind == Tile::Kind::Separator ? separator : base) + spacing;
        total -= spacing;
        float left = (width() - total) / 2;
        float at = left;
        float range = base * 2.6f;
        for (auto& tile : m_tiles) {
            float extent = tile.kind == Tile::Kind::Separator ? separator : base;
            float center = at + extent / 2;
            float size = extent;
            if (tile.kind != Tile::Kind::Separator && m_magnify > 0) {
                float distance = center - m_pointer_x;
                distance = distance < 0 ? -distance : distance;
                float closeness = distance < range ? static_cast<float>(Math::cosine(static_cast<double>(distance / range) * Math::pi / 2)) : 0;
                size = base + (magnified - base) * closeness * closeness * m_magnify;
            }
            tile.size = size;
            tile.center = center;
            at += extent + spacing;
        }
        // Spread out by what they gained, from the middle.
        float grown = 0;
        for (auto const& tile : m_tiles)
            grown += tile.size - (tile.kind == Tile::Kind::Separator ? separator : base);
        float x = left - grown / 2;
        for (auto& tile : m_tiles) {
            tile.center = x + tile.size / 2;
            x += tile.size + spacing;
        }
        float shelf_height = base + padding * 2 + dot;
        float bottom_margin = t.metric("dock.bottomMargin"_sv);
        m_shelf = { left - grown / 2 - padding, height() - bottom_margin - shelf_height, total + grown + padding * 2, shelf_height };
    }

    void update_hover()
    {
        compute_layout();
        int found = -1;
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            auto const& tile = m_tiles[i];
            if (tile.kind == Tile::Kind::Separator)
                continue;
            if (m_pointer_x >= tile.center - tile.size / 2 - 1 && m_pointer_x < tile.center + tile.size / 2 + 1)
                found = static_cast<int>(i);
        }
        m_hovered = found;
    }

    void activate_tile(size_t index)
    {
        auto& tile = m_tiles[index];
        auto& server = Application::the().connection().server();
        switch (tile.kind) {
        case Tile::Kind::Separator:
            return;
        case Tile::Kind::Trash: {
            StringBuilder path;
            path.append(Desktop::home_directory().view());
            path.append("/.Trash"_sv);
            (void)Desktop::open_path(path.view());
            return;
        }
        case Tile::Kind::MinimizedWindow:
            (void)server.activate_window(tile.window_id);
            return;
        case Tile::Kind::Application:
            break;
        }
        if (tile.pid) {
            (void)server.activate_application(tile.pid);
            // Files, with nothing open, opens something.
            if (tile.identifier.view() == "com.vulture.Files"_sv && !m_files_has_windows)
                (void)Desktop::open_path(Desktop::home_directory().view());
            return;
        }
        if (tile.bundle_path.is_empty())
            return;
        printf("dock: opening %s\n", tile.title.characters());
        fflush(stdout);
        (void)Desktop::open_path(tile.bundle_path.view());
        if (motion_enabled()) {
            tile.bouncing_since = Platform::the().now();
            if (m_bounce_timer < 0)
                m_bounce_timer = Platform::the().add_timer(16, true, [this] { animation_frame(); });
        }
    }

    void show_menu(size_t index)
    {
        auto const& tile = m_tiles[index];
        m_menu = adopt_own(*new Menu);
        auto pid = tile.pid;
        auto& server = Application::the().connection().server();
        if (tile.kind == Tile::Kind::Application) {
            if (pid) {
                m_menu->add("Show"_sv, [pid, &server] { (void)server.activate_application(pid); });
                m_menu->add("Hide"_sv, [pid, &server] { (void)server.hide_application(pid, true); });
                m_menu->add_separator();
                m_menu->add("Quit"_sv, [pid, &server] { (void)server.quit_application(pid); });
            } else {
                m_menu->add("Open"_sv, [this, index] { activate_tile(index); });
            }
        } else if (tile.kind == Tile::Kind::Trash) {
            m_menu->add("Open"_sv, [this, index] { activate_tile(index); });
        } else {
            return;
        }
        auto origin = window()->frame().origin();
        auto size = m_menu->size();
        m_menu->open(*window(), { origin.x + static_cast<int>(tile.center - size.width / 2), origin.y + static_cast<int>(m_shelf.y - size.height - 8) });
    }

    Vector<Tile> m_pinned;
    Vector<Tile> m_tiles;
    bool m_files_has_windows { true };
    Vector<RunningApplication> m_state_applications;
    Vector<MinimizedEntry> m_minimized_pending;
    FloatRect m_shelf;
    IntRect m_last_shelf;
    float m_pointer_x { 0 };
    bool m_hovered_inside { false };
    int m_hovered { -1 };
    int m_pressed { -1 };
    float m_magnify { 0 };
    bool m_magnifying_wanted { false };
    Animation::Animator::Id m_magnify_animation { 0 };
    int m_bounce_timer { -1 };
    OwnPtr<Menu> m_menu;
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            s_still = true;
    }
    auto application_or_error = Application::create();
    if (application_or_error.is_error()) {
        fprintf(stderr, "Dock: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    if (s_still)
        set_motion_enabled(false);

    auto screen = Platform::the().screen_size();
    auto const& t = tokens();
    auto height = static_cast<int>(t.metric("dock.windowHeight"_sv));
    auto window = adopt_own(*new UI::Window("Dock"_sv, { 0, screen.height - height, screen.width, height }, WS::WindowBare | WS::WindowSticky));
    window->set_transparent(true);
    window->backend().set_level(WS::LevelDock);
    auto& dock = window->root_view().add<DockView>();
    dock.set_frame({ 0, 0, static_cast<float>(screen.width), static_cast<float>(height) });
    window->show();
    dock.update_shape();

    auto const shelf_reserved = static_cast<int>(t.metric("dock.iconSize"_sv) + t.metric("dock.padding"_sv) * 2 + t.metric("dock.dotSize"_sv) + t.metric("dock.bottomMargin"_sv));
    auto& server = application->connection().server();
    (void)server.set_work_area(static_cast<int>(t.metric("menubar.height"_sv)), 0, shelf_reserved, 0);

    application->connection().on_shell_state = [&dock](StringView json) {
        auto state = Desktop::parse_shell_state(json);
        if (!state.is_error())
            dock.set_state(state.value());
    };
    (void)server.subscribe_shell();
    printf("dock: ready\n");
    fflush(stdout);
    return application->exec();
}
