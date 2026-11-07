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
    if (!m_active_application) {
        m_active_application = client;
        application_became_active(nullptr);
    }
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
    // The desktop's owner: the first who isn't root is whoever's logged in.
    if (!m_has_shell_uid && sender.uid != 0) {
        m_shell_uid = sender.uid;
        m_has_shell_uid = true;
    }
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
    auto* previous_application = m_active_application;
    m_active_application = &client;
    application_became_active(previous_application);
    reveal_desktop(false, true);
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
        show_space_of(*best);
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

void Server::add_menu_extra(IPC::Sender const& sender, u32 id)
{
    auto* window = window_of(sender, id);
    if (!window)
        return;
    for (auto const* known : m_menu_extras) {
        if (known == window)
            return;
    }
    window->level = Window::LevelMenuBar;
    m_windows.remove_all_matching([&](auto* other) { return other == window; });
    insert_by_level(*window);
    m_menu_extras.append(window);
    layout_menu_extras();
}

void Server::set_menu_extras_edge(IPC::Sender const&, i32 x, i32 gap)
{
    m_menu_extras_edge = x;
    m_menu_extras_gap = max(gap, 0);
    layout_menu_extras();
}

void Server::layout_menu_extras()
{
    // From the shell's own leftwards, along the main display's menu bar.
    auto const& display = m_screen.main_display().points;
    int right = display.x + m_menu_extras_edge;
    for (auto* extra : m_menu_extras) {
        right -= extra->frame.width + m_menu_extras_gap;
        IntRect const frame { right, display.y, extra->frame.width, extra->frame.height };
        if (frame != extra->frame) {
            move_window_to(*extra, frame);
            send_moved(*extra);
        }
    }
    shell_state_changed();
}

void Server::show_windows_of(IPC::Sender const&, i32 pid)
{
    auto* client = client_with_pid(pid);
    if (!client)
        return;
    activate_client(*client);
    enter_overview(OverviewMode::OneApplication);
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
        show_space_of(*window);
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

void Server::set_dock_edge(IPC::Sender const&, i32 edge, i32 thickness)
{
    m_dock_edge = clamp(edge, 0, 2);
    m_dock_thickness = max(thickness, 0);
}

void Server::set_badge(IPC::Sender const& sender, String text)
{
    auto* client = client_of(sender);
    if (!client || client->badge.view() == text.view())
        return;
    client->badge = move(text);
    shell_state_changed();
}

void Server::set_minimize_target(IPC::Sender const&, i32 x, i32 y)
{
    m_minimize_target = { x, y };
}

void Server::set_work_area(IPC::Sender const& sender, i32 top, i32 left, i32 bottom, i32 right)
{
    // Each says what it keeps clear (the shell the menu bar's, the Dock its own edge): the most at each edge goes.
    if (auto* client = client_of(sender)) {
        client->work_area_reserved[0] = max(top, 0);
        client->work_area_reserved[1] = max(left, 0);
        client->work_area_reserved[2] = max(bottom, 0);
        client->work_area_reserved[3] = max(right, 0);
    }
    int most[4] {};
    for (auto const& client : m_clients) {
        for (int edge = 0; edge < 4; ++edge)
            most[edge] = max(most[edge], client->work_area_reserved[edge]);
    }
    m_work_top = most[0];
    m_work_left = most[1];
    m_work_bottom = most[2];
    m_work_right = most[3];
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
    {
        auto const& main_spaces = spaces_of(m_screen.main_display());
        MUST(state.set("space"_sv, JSON::Value(main_spaces.current)));
        MUST(state.set("spaces"_sv, JSON::Value(static_cast<int>(main_spaces.spaces.size()))));
    }
    MUST(state.set("stage"_sv, JSON::Value(m_stage)));
    MUST(state.set("desktopRevealed"_sv, JSON::Value(m_desktop_revealed)));
    if (m_switching) {
        auto switcher = JSON::Value::array();
        for (auto* one : applications_most_recent_first())
            MUST(switcher.append(JSON::Value(static_cast<i64>(one->pid))));
        MUST(state.set("switcher"_sv, move(switcher)));
        MUST(state.set("switcherSelected"_sv, JSON::Value(m_switch_selected)));
    }
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
        MUST(application.set("notResponding"_sv, JSON::Value(client->not_responding)));
        MUST(application.set("badge"_sv, MUST(JSON::Value::string(client->badge.view()))));
        auto windows = JSON::Value::array();
        for (auto const* window : m_windows) {
            if (window->client != client.ptr() || !window->is_document_window() || !window->shown_by_client)
                continue;
            auto one = JSON::Value::object();
            MUST(one.set("id"_sv, JSON::Value(static_cast<i64>(window->id))));
            MUST(one.set("title"_sv, MUST(JSON::Value::string(window->title.view()))));
            MUST(one.set("minimized"_sv, JSON::Value(window->minimized)));
            MUST(one.set("space"_sv, JSON::Value(index_of_space(window->space))));
            MUST(one.set("fullScreen"_sv, JSON::Value(window->full_screen)));
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
    bool on_this_space = window.is_on_every_space() || space_is_showing(window.space) || space_is_sliding(window.space);
    if (is_kept_out_of_the_way(window))
        on_this_space = false;
    // In Stage, an application's windows are only there when it's the one that's up.
    if (m_stage && window.level == Window::LevelNormal && window.is_document_window() && window.client != m_stage_shown)
        on_this_space = false;
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
        // It's told the pointer has gone, or it would go on showing what it showed for it (the Dock, magnified).
        if (m_hovered == &window) {
            send_mouse(window, MouseKind::Left, m_pointer, 0);
            m_hovered = nullptr;
        }
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
    if (!picture.is_error() && !m_reduce_motion) {
        Genie genie;
        genie.window = &window;
        genie.picture = picture.release_value();
        genie.from = window.frame.to<float>();
        auto points = m_screen.main_display().points.size();
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
    if (!picture.is_error() && !m_reduce_motion) {
        Genie genie;
        genie.window = &window;
        genie.picture = picture.release_value();
        genie.from = window.frame.to<float>();
        auto points = m_screen.main_display().points.size();
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
    // All of whichever display it's mostly on.
    IntRect const full = work_area_of(m_screen.display_at(window.frame.center()));
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
            if (auto& where = focused_spaces(); where.current > 0)
                go_to_space(where, where.current - 1, true);
            return true;
        case VULTURE_KEY_RIGHT:
            if (auto& where = focused_spaces(); where.current + 1 < static_cast<int>(where.spaces.size()))
                go_to_space(where, where.current + 1, true);
            return true;
        case VULTURE_KEY_DOWN:
            if (m_overview)
                leave_overview(nullptr);
            else
                enter_overview(OverviewMode::OneApplication);
            return true;
        default:
            break;
        }
    }
    if (pressed == (Window::ModifierCommand | Window::ModifierControl) && code == VULTURE_KEY_F) {
        if (m_active && m_active->full_screen)
            leave_full_screen(*m_active, false);
        else if (m_active)
            enter_full_screen(*m_active);
        return true;
    }
    if (m_switching && code == VULTURE_KEY_ESC) {
        finish_switching(false);
        return true;
    }
    if (m_switching && pressed == Window::ModifierCommand && (code == VULTURE_KEY_Q || code == VULTURE_KEY_H)) {
        auto applications = applications_most_recent_first();
        if (m_switch_selected < static_cast<int>(applications.size())) {
            auto* chosen = applications[static_cast<size_t>(m_switch_selected)];
            if (code == VULTURE_KEY_Q) {
                IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::QuitRequested));
                send_to(*chosen, encoder);
            } else {
                chosen->hidden = true;
                update_all_visibility();
                shell_state_changed();
            }
        }
        return true;
    }
    if (pressed == Window::ModifierCommand && code == VULTURE_KEY_TAB) {
        cycle_applications(false);
        return true;
    }
    if (pressed == (Window::ModifierCommand | Window::ModifierShift) && code == VULTURE_KEY_TAB) {
        cycle_applications(true);
        return true;
    }
    if (pressed == Window::ModifierCommand && code == VULTURE_KEY_GRAVE) {
        cycle_windows(false);
        return true;
    }
    if (pressed == (Window::ModifierCommand | Window::ModifierShift) && code == VULTURE_KEY_GRAVE) {
        cycle_windows(true);
        return true;
    }
    return false;
}

void Server::cycle_windows(bool backward)
{
    if (!m_active || !m_active->client)
        return;
    // Of this application, here, bottom to top, as they're stacked.
    Vector<ServerWindow*> own;
    for (auto* window : m_windows) {
        if (window->client == m_active->client && window->visible && window->can_become_key() && window->is_titled())
            own.append(window);
    }
    if (own.size() < 2)
        return;
    // Forward: the one at the bottom comes up. Back: the top one goes to the bottom, by bringing up all the rest in turn.
    if (!backward) {
        activate(own[0]);
        return;
    }
    for (size_t i = own.size() - 1; i > 0; --i)
        raise(*own[i - 1]);
    activate(own[own.size() - 2]);
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

// ---- Time passing

void Server::animation_tick()
{
    double now = now_seconds();
    bool still_going = false;

    // Genies.
    if (!m_genies.is_empty()) {
        Vector<Compositor::Overlay> overlays = lasting_overlays();
        for (size_t i = 0; i < m_genies.size();) {
            auto& genie = m_genies[i];
            auto t = static_cast<float>((now - genie.started) / static_cast<double>(m_theme.minimize_seconds));
            // Its window's whole travel, both ways.
            IntRect reach = genie.from.enclosing_int_rect();
            int scale = m_screen.scale();
            auto const points = m_screen.main_display().points.size();
            auto const origin = m_screen.origin();
            IntRect area { (reach.x + origin.x) * scale, (reach.y + origin.y) * scale, reach.width * scale, (points.height - reach.y) * scale };
            area.x = min(area.x, (static_cast<int>(genie.to.x) + origin.x) * scale - 16);
            area.width = max((reach.right() + origin.x) * scale, (static_cast<int>(genie.to.x) + origin.x) * scale + 16) - area.x;
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
        m_compositor.set_overlays(lasting_overlays());
    }

    // Sliding between Spaces; what comes into view on a full screen one; the desktop being uncovered.
    still_going = advance_slide(now) || still_going;
    still_going = advance_reveals(now) || still_going;
    still_going = advance_desktop_reveal(now) || still_going;

    // Overview.
    if (m_overview) {
        bool finished = m_reduce_motion || now - m_overview_started >= static_cast<double>(m_theme.overview_seconds);
        if (advance_overview_bar(now))
            still_going = true;
        update_overview_overlays();
        if (finished && m_overview_leaving) {
            m_overview = false;
            m_overview_leaving = false;
            slide_menu_bars(0);
            m_compositor.set_hides_normal_windows(false);
            m_compositor.set_overlays(lasting_overlays());
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
    auto points = m_screen.main_display().points.size();
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
