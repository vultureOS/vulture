/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Server.h"
#include <Kernel/API/POSIX/input.h>
#include <LibAnimation/Curve.h>
#include <LibBase/Math.h>
#include <LibJSON/JSON.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// The part of the server that makes a desktop out of windows: applications and their
// menus, as told to the Dock and menu bar; minimizing; spaces; Overview. The
// programs of the desktop itself are clients like any other.

namespace WindowServer {

using namespace Gfx;
using Window::MouseKind;
namespace Math = Base::Math;

namespace {

double now_seconds()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1'000'000'000.0;
}

ErrorOr<Vector<u8>> read_whole_file(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    u8 chunk[16384];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) {
        size_t at = bytes.size();
        TRY(bytes.try_resize(at + static_cast<size_t>(got)));
        memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return bytes;
}

ErrorOr<Color> parse_hex_color(StringView text)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    if (text.length() != 7 || text[0] != '#')
        return Error::from_errno(EINVAL);
    u8 values[3];
    for (size_t i = 0; i < 3; ++i) {
        int high = hex(text[1 + i * 2]);
        int low = hex(text[2 + i * 2]);
        if (high < 0 || low < 0)
            return Error::from_errno(EINVAL);
        values[i] = static_cast<u8>(high * 16 + low);
    }
    return Color { values[0], values[1], values[2], 255 };
}

float ease_out(float t)
{
    float inverse = 1 - t;
    return 1 - inverse * inverse * inverse;
}

FloatRect between(FloatRect const& a, FloatRect const& b, float t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.width + (b.width - a.width) * t, a.height + (b.height - a.height) * t };
}

}

Client* Server::client_with_pid(i32 pid)
{
    for (auto& client : m_clients) {
        if (client->pid == pid)
            return client.ptr();
    }
    return nullptr;
}

void Server::set_application(IPC::Sender const& sender, String name, String identifier)
{
    auto* client = client_of(sender);
    if (!client)
        return;
    client->application_name = move(name);
    client->application_identifier = move(identifier);
    if (!m_active_application)
        m_active_application = client;
    shell_state_changed();
}

void Server::set_menus(IPC::Sender const& sender, String menus)
{
    auto* client = client_of(sender);
    if (!client)
        return;
    client->menus = move(menus);
    shell_state_changed();
}

void Server::subscribe_shell(IPC::Sender const& sender)
{
    auto* client = client_of(sender);
    if (!client)
        return;
    client->is_shell = true;
    shell_state_changed();
}

void Server::choose_menu_item(IPC::Sender const&, i32 pid, u32 item)
{
    auto* client = client_with_pid(pid);
    if (!client)
        return;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::MenuItemChosen));
    encoder.encode(item);
    send_to(*client, encoder);
}

void Server::activate_client(Client& client)
{
    m_active_application = &client;
    if (client.hidden) {
        client.hidden = false;
        update_all_visibility();
    }
    // Its frontmost window, wherever that is; if all are minimized, the last.
    ServerWindow* best = nullptr;
    ServerWindow* minimized = nullptr;
    for (auto* window : m_windows) {
        if (window->client != &client || !window->can_become_key() || !window->shown_by_client)
            continue;
        if (window->minimized)
            minimized = window;
        else
            best = window;
    }
    if (!best && minimized) {
        restore_window(*minimized);
        best = minimized;
    }
    if (best) {
        if (!best->is_on_every_space() && best->space != m_space)
            go_to_space(best->space, true);
        activate(best);
    } else if (m_active) {
        // No windows: it is the active application, with its menus, and no window.
        m_compositor.invalidate_window(*m_active);
        m_active->active = false;
        m_active->chrome_is_stale = true;
        m_compositor.invalidate_window(*m_active);
        if (m_active->client) {
            IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Activated));
            encoder.encode(m_active->id);
            encoder.encode(false);
            send_to(*m_active->client, encoder);
        }
        m_active = nullptr;
    }
    shell_state_changed();
    schedule_frame();
}

void Server::activate_application(IPC::Sender const&, i32 pid)
{
    if (auto* client = client_with_pid(pid))
        activate_client(*client);
}

void Server::hide_application(IPC::Sender const&, i32 pid, bool hidden)
{
    auto* client = client_with_pid(pid);
    if (!client || client->hidden == hidden)
        return;
    client->hidden = hidden;
    update_all_visibility();
    if (hidden && m_active && m_active->client == client) {
        m_active->active = false;
        m_active = nullptr;
        activate_frontmost();
    }
    shell_state_changed();
    schedule_frame();
}

void Server::quit_application(IPC::Sender const&, i32 pid)
{
    auto* client = client_with_pid(pid);
    if (!client)
        return;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::QuitRequested));
    send_to(*client, encoder);
}

void Server::activate_window(IPC::Sender const&, u32 id)
{
    for (auto* window : m_windows) {
        if (window->id != id)
            continue;
        if (window->client && window->client->hidden) {
            window->client->hidden = false;
            update_all_visibility();
        }
        if (window->minimized)
            restore_window(*window);
        if (!window->is_on_every_space() && window->space != m_space)
            go_to_space(window->space, true);
        if (window->can_become_key())
            activate(window);
        shell_state_changed();
        schedule_frame();
        return;
    }
}

void Server::minimize(IPC::Sender const& sender, u32 id)
{
    if (auto* window = window_of(sender, id))
        minimize_window(*window);
}

void Server::zoom(IPC::Sender const& sender, u32 id)
{
    if (auto* window = window_of(sender, id))
        zoom_window(*window);
}

void Server::set_minimize_target(IPC::Sender const&, i32 x, i32 y)
{
    m_minimize_target = { x, y };
}

void Server::set_work_area(IPC::Sender const&, i32 top, i32 left, i32 bottom, i32 right)
{
    m_work_top = max(top, 0);
    m_work_left = max(left, 0);
    m_work_bottom = max(bottom, 0);
    m_work_right = max(right, 0);
}

void Server::register_hotkey(IPC::Sender const& sender, u16 code, u32 modifiers)
{
    auto* client = client_of(sender);
    if (client)
        m_hotkeys.append(Hotkey { code, modifiers, client });
}

i32 Server::set_wallpaper(IPC::Sender const&, String name)
{
    m_wallpaper_name = move(name);
    apply_wallpaper();
    schedule_frame();
    return 0;
}

void Server::apply_wallpaper()
{
    // The name's colors in this appearance. Without a name, the theme's own.
    if (m_wallpaper_name.is_empty())
        return;
    auto bytes = read_whole_file("/System/Library/Themes/Wallpapers.json");
    if (bytes.is_error())
        return;
    auto json = JSON::parse(StringView(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
    if (json.is_error())
        return;
    auto const* entry = json.value().get_object(m_wallpaper_name.view());
    if (!entry)
        return;
    auto const* colors = entry->get_array(m_appearance.view() == "Dark"_sv ? "dark"_sv : "light"_sv);
    if (!colors)
        return;
    Vector<Color> parsed;
    for (size_t i = 0; i < colors->size(); ++i) {
        if (!colors->at(i).is_string())
            return;
        auto color = parse_hex_color(colors->at(i).as_string());
        if (color.is_error())
            return;
        parsed.append(color.value());
    }
    m_compositor.set_wallpaper_colors(move(parsed));
}

// ---- What the shell hears

void Server::shell_state_changed()
{
    if (m_shell_state_pending)
        return;
    bool any = false;
    for (auto const& client : m_clients) {
        if (client->is_shell)
            any = true;
    }
    if (!any)
        return;
    m_shell_state_pending = true;
    m_loop.deferred_invoke([this] {
        m_shell_state_pending = false;
        send_shell_state();
    });
}

void Server::send_shell_state()
{
    auto state = JSON::Value::object();
    auto* active = m_active_application;
    MUST(state.set("active"_sv, JSON::Value(static_cast<int>(active ? active->pid : 0))));
    if (active) {
        MUST(state.set("activeName"_sv, MUST(JSON::Value::string(active->application_name.view()))));
        MUST(state.set("activeIdentifier"_sv, MUST(JSON::Value::string(active->application_identifier.view()))));
        MUST(state.set("menus"_sv, MUST(JSON::Value::string(active->menus.view()))));
    }
    MUST(state.set("space"_sv, JSON::Value(m_space)));
    MUST(state.set("spaces"_sv, JSON::Value(m_space_count)));
    MUST(state.set("overview"_sv, JSON::Value(m_overview)));
    auto applications = JSON::Value::array();
    for (auto const& client : m_clients) {
        if (client->application_name.is_empty())
            continue;
        auto application = JSON::Value::object();
        MUST(application.set("pid"_sv, JSON::Value(static_cast<int>(client->pid))));
        MUST(application.set("name"_sv, MUST(JSON::Value::string(client->application_name.view()))));
        MUST(application.set("identifier"_sv, MUST(JSON::Value::string(client->application_identifier.view()))));
        MUST(application.set("hidden"_sv, JSON::Value(client->hidden)));
        auto windows = JSON::Value::array();
        for (auto const* window : m_windows) {
            if (window->client != client.ptr() || !window->is_document_window() || !window->shown_by_client)
                continue;
            auto one = JSON::Value::object();
            MUST(one.set("id"_sv, JSON::Value(static_cast<i64>(window->id))));
            MUST(one.set("title"_sv, MUST(JSON::Value::string(window->title.view()))));
            MUST(one.set("minimized"_sv, JSON::Value(window->minimized)));
            MUST(one.set("space"_sv, JSON::Value(window->space)));
            MUST(windows.append(move(one)));
        }
        MUST(application.set("windows"_sv, move(windows)));
        MUST(applications.append(move(application)));
    }
    MUST(state.set("applications"_sv, move(applications)));
    auto text = MUST(state.serialize());
    for (auto& client : m_clients) {
        if (!client->is_shell)
            continue;
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::ShellState));
        encoder.encode(text.view());
        send_to(*client, encoder);
    }
}

// ---- Visibility

void Server::update_visibility(ServerWindow& window)
{
    bool on_this_space = window.is_on_every_space() || window.space == m_space || m_sliding;
    bool hidden_with_application = window.client && window.client->hidden && window.can_become_key();
    bool should = window.shown_by_client && !window.minimized && !hidden_with_application && on_this_space;
    if (window.visible == should)
        return;
    m_compositor.invalidate_window(window);
    window.visible = should;
    if (should)
        window.chrome_is_stale = true;
    m_compositor.invalidate_window(window);
    if (!should) {
        if (m_grab == &window)
            m_grab = nullptr;
        if (m_hovered == &window)
            m_hovered = nullptr;
        if (m_active == &window) {
            m_compositor.invalidate_window(window);
            window.active = false;
            window.chrome_is_stale = true;
            m_compositor.invalidate_window(window);
            m_active = nullptr;
            activate_frontmost();
        }
    }
}

void Server::update_all_visibility()
{
    for (size_t i = 0; i < m_windows.size(); ++i)
        update_visibility(*m_windows[i]);
    schedule_frame();
}

// ---- Minimizing, with a genie

void Server::start_animating()
{
    if (m_animation_timer >= 0)
        return;
    m_animation_timer = m_loop.add_timer(16, true, [this] { animation_tick(); });
}

void Server::minimize_window(ServerWindow& window)
{
    if (window.minimized)
        return;
    if (window.chrome_is_stale && window.is_titled())
        render_chrome(window);
    auto picture = m_compositor.snapshot(window);
    window.minimized = true;
    update_visibility(window);
    if (!picture.is_error()) {
        Genie genie;
        genie.window = &window;
        genie.picture = picture.release_value();
        genie.from = window.frame.to<float>();
        auto points = m_screen.points();
        genie.to = m_minimize_target == IntPoint { 0, 0 } ? FloatPoint { static_cast<float>(points.width) / 2, static_cast<float>(points.height) } : m_minimize_target.to<float>();
        genie.started = now_seconds();
        m_genies.append(move(genie));
        start_animating();
    }
    shell_state_changed();
    schedule_frame();
}

void Server::restore_window(ServerWindow& window)
{
    if (!window.minimized)
        return;
    if (window.chrome_is_stale && window.is_titled())
        render_chrome(window);
    auto picture = m_compositor.snapshot(window);
    if (!picture.is_error()) {
        Genie genie;
        genie.window = &window;
        genie.picture = picture.release_value();
        genie.from = window.frame.to<float>();
        auto points = m_screen.points();
        genie.to = m_minimize_target == IntPoint { 0, 0 } ? FloatPoint { static_cast<float>(points.width) / 2, static_cast<float>(points.height) } : m_minimize_target.to<float>();
        genie.started = now_seconds();
        genie.restoring = true;
        m_genies.append(move(genie));
        start_animating();
    } else {
        window.minimized = false;
        update_visibility(window);
    }
    shell_state_changed();
    schedule_frame();
}

void Server::zoom_window(ServerWindow& window)
{
    auto points = m_screen.points();
    IntRect full { m_work_left, m_work_top, points.width - m_work_left - m_work_right, points.height - m_work_top - m_work_bottom };
    // Back to what it was, if it is already zoomed.
    if (window.frame == full && !window.zoom_restore.is_empty()) {
        move_window_to(window, window.zoom_restore);
    } else {
        window.zoom_restore = window.frame;
        move_window_to(window, full);
    }
    send_moved(window);
    send_resized(window);
}

// ---- Hotkeys

bool Server::handle_hotkey(u16 code)
{
    u32 pressed = m_modifiers & (Window::ModifierShift | Window::ModifierControl | Window::ModifierOption | Window::ModifierCommand);
    for (auto const& hotkey : m_hotkeys) {
        if (hotkey.code == code && hotkey.modifiers == pressed && hotkey.client) {
            IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Hotkey));
            encoder.encode(code);
            encoder.encode(pressed);
            send_to(*hotkey.client, encoder);
            return true;
        }
    }
    if (m_overview && code == VULTURE_KEY_ESC) {
        leave_overview(nullptr);
        return true;
    }
    if (pressed == Window::ModifierControl) {
        switch (code) {
        case VULTURE_KEY_UP:
            if (m_overview)
                leave_overview(nullptr);
            else
                enter_overview();
            return true;
        case VULTURE_KEY_LEFT:
            if (m_space > 0)
                go_to_space(m_space - 1, true);
            return true;
        case VULTURE_KEY_RIGHT:
            if (m_space + 1 < m_space_count)
                go_to_space(m_space + 1, true);
            return true;
        default:
            break;
        }
    }
    if (pressed == Window::ModifierCommand && code == VULTURE_KEY_TAB) {
        cycle_applications(false);
        return true;
    }
    if (pressed == (Window::ModifierCommand | Window::ModifierShift) && code == VULTURE_KEY_TAB) {
        cycle_applications(true);
        return true;
    }
    return false;
}

void Server::cycle_applications(bool backward)
{
    Vector<Client*> applications;
    for (auto& client : m_clients) {
        if (!client->application_name.is_empty())
            applications.append(client.ptr());
    }
    if (applications.size() < 2)
        return;
    size_t at = 0;
    for (size_t i = 0; i < applications.size(); ++i) {
        if (applications[i] == m_active_application)
            at = i;
    }
    size_t next = backward ? (at + applications.size() - 1) % applications.size() : (at + 1) % applications.size();
    activate_client(*applications[next]);
}

// ---- Desktops

void Server::set_overview(IPC::Sender const&, bool shown)
{
    if (shown && !m_overview)
        enter_overview();
    else if (!shown && m_overview)
        leave_overview(nullptr);
}

void Server::set_input_region(IPC::Sender const& sender, u32 id, i32 x, i32 y, i32 width, i32 height)
{
    if (auto* window = window_of(sender, id))
        window->input_region = { x, y, width, height };
}

void Server::switch_space(IPC::Sender const&, i32 space)
{
    if (space < 0)
        return;
    if (space >= m_space_count)
        m_space_count = space + 1;
    go_to_space(space, true);
}

void Server::go_to_space(int space, bool animated)
{
    if (space == m_space)
        return;
    m_slide_from = m_space;
    m_space = space;
    m_sliding = animated;
    m_slide_started = now_seconds();
    if (animated)
        start_animating();
    update_all_visibility();
    // The new space's frontmost has the keyboard, or nothing does.
    if (!animated) {
        if (m_active && !m_active->is_on_every_space() && m_active->space != m_space)
            activate(nullptr);
        activate_frontmost();
    }
    shell_state_changed();
    schedule_frame();
}

// ---- Overview

void Server::layout_overview()
{
    // The windows of this space in a grid, in the work area, under the bar of spaces.
    auto points = m_screen.points();
    float top = static_cast<float>(m_work_top) + m_theme.overview_bar;
    float bottom = static_cast<float>(points.height - m_work_bottom);
    float left = static_cast<float>(m_work_left);
    float right = static_cast<float>(points.width - m_work_right);
    float padding = m_theme.overview_padding;
    size_t count = m_thumbnails.size();
    if (count == 0)
        return;
    size_t columns = 1;
    while (columns * columns < count)
        ++columns;
    size_t rows = (count + columns - 1) / columns;
    float cell_width = (right - left) / static_cast<float>(columns);
    float cell_height = (bottom - top) / static_cast<float>(rows);
    for (size_t i = 0; i < count; ++i) {
        auto& thumbnail = m_thumbnails[i];
        size_t column = i % columns;
        size_t row = i / columns;
        float available_width = cell_width - padding * 2;
        float available_height = cell_height - padding * 2;
        float scale = min(min(available_width / thumbnail.home.width, available_height / thumbnail.home.height), 1.0f);
        float width = thumbnail.home.width * scale;
        float height = thumbnail.home.height * scale;
        thumbnail.spread = { left + cell_width * static_cast<float>(column) + (cell_width - width) / 2, top + cell_height * static_cast<float>(row) + (cell_height - height) / 2, width, height };
    }
}

void Server::enter_overview()
{
    m_thumbnails.clear();
    for (auto* window : m_windows) {
        if (!window->visible || window->level != Window::LevelNormal || window->is_popup())
            continue;
        if (window->chrome_is_stale && window->is_titled())
            render_chrome(*window);
        auto picture = m_compositor.snapshot(*window);
        if (picture.is_error())
            continue;
        Thumbnail thumbnail;
        thumbnail.window = window;
        thumbnail.picture = picture.release_value();
        thumbnail.home = window->frame.to<float>();
        thumbnail.spread = thumbnail.home;
        m_thumbnails.append(move(thumbnail));
    }
    layout_overview();
    m_overview = true;
    m_overview_leaving = false;
    m_overview_chosen = nullptr;
    m_overview_started = now_seconds();
    m_overview_hover = -1;
    m_overview_hover_space = -1;
    m_compositor.set_hides_normal_windows(true);
    start_animating();
    m_compositor.invalidate_all();
    shell_state_changed();
    schedule_frame();
}

void Server::leave_overview(ServerWindow* to_activate)
{
    if (!m_overview || m_overview_leaving)
        return;
    m_overview_leaving = true;
    m_overview_chosen = to_activate;
    m_overview_started = now_seconds();
    start_animating();
}

void Server::update_overview_overlays()
{
    float duration = m_theme.overview_seconds;
    auto t = static_cast<float>((now_seconds() - m_overview_started) / static_cast<double>(duration));
    t = clamp(t, 0.0f, 1.0f);
    float amount = ease_out(t);
    if (m_overview_leaving)
        amount = 1 - amount;
    Vector<Compositor::Overlay> overlays;

    // Desktops along the top.
    auto points = m_screen.points();
    float tile_width = m_theme.overview_tile_width;
    float tile_height = m_theme.overview_tile_height;
    float gap = m_theme.overview_padding;
    int tiles = m_space_count + 1;
    float total = static_cast<float>(tiles) * tile_width + static_cast<float>(tiles - 1) * gap;
    float x = (static_cast<float>(points.width) - total) / 2;
    float y = (m_theme.overview_bar - tile_height) / 2 + static_cast<float>(m_work_top) - (1 - amount) * m_theme.overview_bar;
    for (int i = 0; i < tiles; ++i) {
        Compositor::Overlay tile;
        tile.kind = Compositor::Overlay::Kind::Tile;
        tile.rect = { x + static_cast<float>(i) * (tile_width + gap), y, tile_width, tile_height };
        tile.radius = m_theme.overview_tile_radius;
        bool current = i == m_space;
        bool hovered = i == m_overview_hover_space;
        tile.color = current ? m_theme.overview_tile_active : (hovered ? m_theme.overview_highlight : m_theme.overview_tile);
        StringBuilder label;
        if (i == m_space_count)
            label.append("+"_sv);
        else
            label.appendff("Desktop {}", i + 1);
        tile.label = MUST(label.to_string());
        overlays.append(move(tile));
    }

    for (size_t i = 0; i < m_thumbnails.size(); ++i) {
        auto const& thumbnail = m_thumbnails[i];
        auto rect = between(thumbnail.home, thumbnail.spread, amount);
        if (static_cast<int>(i) == m_overview_hover) {
            Compositor::Overlay ring;
            ring.kind = Compositor::Overlay::Kind::Tile;
            ring.rect = rect.inflated(3);
            ring.radius = m_theme.corner_radius + 3;
            ring.color = m_theme.overview_highlight;
            overlays.append(move(ring));
        }
        Compositor::Overlay overlay;
        overlay.kind = Compositor::Overlay::Kind::Thumbnail;
        overlay.picture = thumbnail.picture;
        overlay.rect = rect;
        overlay.radius = m_theme.corner_radius;
        overlay.shadow = true;
        overlays.append(move(overlay));
    }
    m_compositor.set_dim(m_theme.overview_dim.with_opacity(amount));
    m_compositor.set_overlays(move(overlays));
    m_compositor.invalidate_all();
}

bool Server::overview_pointer(IntPoint point, u32 pressed)
{
    if (m_overview_leaving)
        return true;
    auto points = m_screen.points();
    // What it is over: a space along the top, or a window.
    float tile_width = m_theme.overview_tile_width;
    float tile_height = m_theme.overview_tile_height;
    float gap = m_theme.overview_padding;
    int tiles = m_space_count + 1;
    float total = static_cast<float>(tiles) * tile_width + static_cast<float>(tiles - 1) * gap;
    float x = (static_cast<float>(points.width) - total) / 2;
    float y = (m_theme.overview_bar - tile_height) / 2 + static_cast<float>(m_work_top);
    int over_space = -1;
    for (int i = 0; i < tiles; ++i) {
        FloatRect tile { x + static_cast<float>(i) * (tile_width + gap), y, tile_width, tile_height };
        if (tile.contains(point.to<float>()))
            over_space = i;
    }
    int over_window = -1;
    for (size_t i = 0; i < m_thumbnails.size(); ++i) {
        if (m_thumbnails[i].spread.contains(point.to<float>()))
            over_window = static_cast<int>(i);
    }
    if (over_space != m_overview_hover_space || over_window != m_overview_hover) {
        m_overview_hover_space = over_space;
        m_overview_hover = over_window;
        update_overview_overlays();
    }
    if (!pressed)
        return true;
    if (over_space == m_space_count) {
        ++m_space_count;
        update_overview_overlays();
        shell_state_changed();
        return true;
    }
    if (over_space >= 0) {
        go_to_space(over_space, false);
        // Its windows now.
        leave_overview(nullptr);
        return true;
    }
    if (over_window >= 0) {
        leave_overview(m_thumbnails[static_cast<size_t>(over_window)].window);
        return true;
    }
    leave_overview(nullptr);
    return true;
}

// ---- Time passing

void Server::animation_tick()
{
    double now = now_seconds();
    bool still_going = false;

    // Genies.
    if (!m_genies.is_empty()) {
        Vector<Compositor::Overlay> overlays;
        for (size_t i = 0; i < m_genies.size();) {
            auto& genie = m_genies[i];
            auto t = static_cast<float>((now - genie.started) / static_cast<double>(m_theme.minimize_seconds));
            // Its window's whole travel, both ways.
            IntRect reach = genie.from.enclosing_int_rect();
            int scale = m_screen.scale();
            auto points = m_screen.points();
            IntRect area { reach.x * scale, reach.y * scale, reach.width * scale, (points.height - reach.y) * scale };
            area.x = min(area.x, static_cast<int>(genie.to.x) * scale - 16);
            area.width = max(reach.right() * scale, static_cast<int>(genie.to.x) * scale + 16) - area.x;
            m_compositor.invalidate(area);
            if (t >= 1) {
                if (genie.restoring && genie.window) {
                    genie.window->minimized = false;
                    update_visibility(*genie.window);
                    activate(genie.window);
                }
                m_genies.remove(i);
                continue;
            }
            Compositor::Overlay overlay;
            overlay.kind = Compositor::Overlay::Kind::Genie;
            overlay.picture = genie.picture;
            overlay.rect = genie.from;
            overlay.target = genie.to;
            overlay.progress = genie.restoring ? 1 - t : t;
            overlays.append(move(overlay));
            ++i;
        }
        still_going = still_going || !m_genies.is_empty();
        if (!m_overview)
            m_compositor.set_overlays(move(overlays));
    } else if (!m_overview && !m_boot_screen) {
        m_compositor.set_overlays({});
    }

    // Sliding between spaces.
    if (m_sliding) {
        auto t = static_cast<float>((now - m_slide_started) / static_cast<double>(m_theme.space_slide_seconds));
        int width = m_screen.pixels().width;
        int direction = m_space > m_slide_from ? 1 : -1;
        if (t >= 1) {
            m_sliding = false;
            for (auto* window : m_windows)
                window->slide_x = 0;
            update_all_visibility();
            if (m_active && !m_active->is_on_every_space() && m_active->space != m_space)
                activate(nullptr);
            if (!m_active)
                activate_frontmost();
        } else {
            float travel = ease_out(t);
            for (auto* window : m_windows) {
                if (window->is_on_every_space())
                    continue;
                // Those leaving slide off; those coming, in from the other side.
                float from_new = window->space == m_space ? 1 - travel : -travel;
                window->slide_x = static_cast<int>(static_cast<float>(direction * width) * from_new);
            }
            still_going = true;
        }
        m_compositor.invalidate_all();
    }

    // Overview.
    if (m_overview) {
        bool finished = now - m_overview_started >= static_cast<double>(m_theme.overview_seconds);
        update_overview_overlays();
        if (finished && m_overview_leaving) {
            m_overview = false;
            m_overview_leaving = false;
            m_compositor.set_hides_normal_windows(false);
            m_compositor.set_overlays({});
            m_compositor.set_dim(Color { 0, 0, 0, 0 });
            auto* chosen = m_overview_chosen;
            m_thumbnails.clear();
            if (chosen)
                activate(chosen);
            m_compositor.invalidate_all();
            shell_state_changed();
        } else if (!finished) {
            still_going = true;
        }
    }

    // The boot screen.
    if (m_boot_screen) {
        update_boot_overlays();
        still_going = m_boot_screen;
    }

    if (!still_going && m_animation_timer >= 0) {
        m_loop.cancel_timer(m_animation_timer);
        m_animation_timer = -1;
    }
    schedule_frame();
}

// ---- Coming up

void Server::show_boot_screen()
{
    m_boot_screen = true;
    m_boot_started = now_seconds();
    m_compositor.set_hides_normal_windows(true);
    update_boot_overlays();
    start_animating();
    schedule_frame();
}

void Server::update_boot_overlays()
{
    double now = now_seconds();
    if (m_boot_finishing && m_boot_finished_at == 0)
        m_boot_finished_at = now;
    // A slow crawl while waiting, then to the end; never past a short time for nothing.
    auto waited = static_cast<float>(now - m_boot_started);
    float progress = 0.9f * ease_out(min(waited / m_theme.boot_seconds, 1.0f));
    if (m_boot_finishing) {
        auto since = static_cast<float>(now - m_boot_finished_at);
        progress = progress + (1 - progress) * min(since / 0.25f, 1.0f);
        if (since >= 0.35f) {
            m_boot_screen = false;
            m_compositor.set_hides_normal_windows(false);
            m_compositor.set_dim(Color { 0, 0, 0, 0 });
            m_compositor.set_overlays({});
            m_compositor.invalidate_all();
            return;
        }
    }
    auto points = m_screen.points();
    float width = m_theme.boot_bar_width;
    float height = m_theme.boot_bar_height;
    float x = (static_cast<float>(points.width) - width) / 2;
    float y = static_cast<float>(points.height) * 0.62f;
    Vector<Compositor::Overlay> overlays;
    Compositor::Overlay logo;
    logo.kind = Compositor::Overlay::Kind::Logo;
    float logo_size = m_theme.boot_logo;
    logo.rect = { (static_cast<float>(points.width) - logo_size) / 2, y - logo_size - 56, logo_size, logo_size };
    logo.color = m_theme.boot_foreground;
    overlays.append(move(logo));
    Compositor::Overlay track;
    track.kind = Compositor::Overlay::Kind::Tile;
    track.rect = { x, y, width, height };
    track.radius = height / 2;
    track.color = m_theme.boot_track;
    overlays.append(move(track));
    Compositor::Overlay bar;
    bar.kind = Compositor::Overlay::Kind::Tile;
    bar.rect = { x, y, max(width * progress, height), height };
    bar.radius = height / 2;
    bar.color = m_theme.boot_foreground;
    overlays.append(move(bar));
    m_compositor.set_dim(m_theme.boot_background);
    m_compositor.set_overlays(move(overlays));
    m_compositor.invalidate_all();
}

}
