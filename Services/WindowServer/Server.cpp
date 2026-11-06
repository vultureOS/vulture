/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <os/log.h>
#include "Server.h"
#include "Cursors.h"
#include <LibJSON/JSON.h>
#include <Kernel/API/POSIX/input.h>
#include <LibBase/Math.h>
#include <LibGfx/Painter.h>
#include <LibImage/Image.h>
#include <LibWindow/Protocol.h>
#include <WindowServerEndpoint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <unistd.h>

namespace {

os_log_t server_log()
{
    static os_log_t const log = os_log_create("com.vulture.windowserver", "clients");
    return log;
}

}

namespace WindowServer {

using namespace Gfx;
using Window::Material;
using Window::MouseKind;

namespace {

constexpr int minimum_width = 120;
constexpr int minimum_height = 60;
// How far from a window's edge (points) a press resizes it.
constexpr int resize_grab = 4;
constexpr int edge_left = 1, edge_right = 2, edge_top = 4, edge_bottom = 8;

u64 now_microseconds()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1'000'000 + static_cast<u64>(now.tv_nsec) / 1000;
}

constexpr u64 frame_interval = 16667;
// How long a program has to answer, with input waiting for it, before it's said not to be responding.
constexpr u64 not_responding_after = 2'000'000;


}

Server::Server(Core::EventLoop& loop, Screen& screen, Theme& theme, Compositor& compositor, Vector<ServerWindow*>& windows, NonnullRefPtr<Text::Typeface> title_face, StringView appearance)
    : m_loop(loop)
    , m_screen(screen)
    , m_theme(theme)
    , m_appearance(MUST(String::from_view(appearance)))
    , m_compositor(compositor)
    , m_windows(windows)
    , m_title_face(move(title_face))
{
    m_title_font = adopt_own(*new Text::Font(m_title_face, theme.title_font_size, static_cast<float>(screen.scale()), m_atlas));
    m_compositor.set_overlay_font(m_title_font.ptr());
    m_small_label_font = adopt_own(*new Text::Font(m_title_face, 11.0f, static_cast<float>(screen.scale()), m_atlas));
    m_compositor.set_overlay_small_font(m_small_label_font.ptr());
    auto points = screen.main_display().points.size();
    m_pointer = { points.width / 2, points.height / 2 };
}

void Server::start()
{
    sync_display_spaces();
    apply_wallpaper();
    schedule_frame();
}

void Server::watch_input(int fd)
{
    m_input_fd = fd;
    m_loop.watch(fd, Core::EventLoop::Interest::Read, [this] { read_input(); });
}

void Server::set_up_cursor()
{
    auto cursor = make_cursor(m_cursor_shape, m_screen.scale(), m_cursor_turn);
    m_screen.set_cursor(*cursor.image, cursor.hot_spot);
    m_screen.move_cursor({ (m_pointer.x + m_screen.origin().x) * m_screen.scale(), (m_pointer.y + m_screen.origin().y) * m_screen.scale() });
}

void Server::update_cursor()
{
    using Window::CursorShape;
    CursorShape wanted = CursorShape::Arrow;
    int edges = 0;
    if (m_dragging_divider || (!window_at(m_pointer) && split_divider_at(m_pointer).has_value())) {
        wanted = CursorShape::ResizeLeftRight;
    } else if (m_drag) {
        wanted = CursorShape::ClosedHand;
    } else if (m_gesture == Gesture::Resizing) {
        edges = m_gesture_edges;
    } else if (m_gesture == Gesture::ContentPress && m_gesture_window) {
        wanted = m_gesture_window->cursor;
    } else if (m_grab) {
        wanted = m_grab->cursor;
    } else if (auto* window = window_at(m_pointer)) {
        edges = resize_edges_at(*window, m_pointer);
        wanted = window->cursor;
        // Whatever it asked for, from when it still could. (The window can still be moved and resized: that's here.)
        if (window->client && window->client->not_responding && !edges)
            wanted = CursorShape::Wait;
    }
    if (edges) {
        bool horizontal = edges & (edge_left | edge_right);
        bool vertical = edges & (edge_top | edge_bottom);
        if (horizontal && vertical)
            wanted = ((edges & edge_left) != 0) == ((edges & edge_top) != 0) ? CursorShape::ResizeDiagonalDown : CursorShape::ResizeDiagonalUp;
        else
            wanted = horizontal ? CursorShape::ResizeLeftRight : CursorShape::ResizeUpDown;
    }
    if (wanted == m_cursor_shape)
        return;
    m_cursor_shape = wanted;
    set_up_cursor();
    // It goes round for as long as it's there, whatever else is going on.
    if (wanted == CursorShape::Wait && m_cursor_timer < 0) {
        m_cursor_timer = m_loop.add_timer(1000 / wait_cursor_steps, true, [this] {
            m_cursor_turn = (m_cursor_turn + 1) % wait_cursor_steps;
            set_up_cursor();
        });
    } else if (wanted != CursorShape::Wait && m_cursor_timer >= 0) {
        m_loop.cancel_timer(m_cursor_timer);
        m_cursor_timer = -1;
        m_cursor_turn = 0;
    }
}

void Server::ask_if_responding(Client& client)
{
    u64 const now = now_microseconds();
    // One question at a time; and for something answered just now, no need to ask again for each move of the pointer.
    if (client.ping_sent_at != 0 || now - client.answered_at < 500'000)
        return;
    client.ping_sent_at = now;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Ping));
    encoder.encode(++client.ping_serial);
    send_to(client, encoder);
    if (m_responding_timer < 0)
        m_responding_timer = m_loop.add_timer(250, true, [this] { check_who_is_responding(); });
}

void Server::check_who_is_responding()
{
    u64 const now = now_microseconds();
    bool waiting = false;
    bool changed = false;
    for (auto& client : m_clients) {
        if (client->ping_sent_at == 0)
            continue;
        waiting = true;
        if (!client->not_responding && now - client->ping_sent_at >= not_responding_after) {
            client->not_responding = true;
            changed = true;
            os_log_info(server_log(), "Process %d isn't responding", client->pid);
        }
    }
    if (changed) {
        update_cursor();
        shell_state_changed();
    }
    if (!waiting) {
        m_loop.cancel_timer(m_responding_timer);
        m_responding_timer = -1;
    }
}

void Server::pong(IPC::Sender const& sender, u32 serial)
{
    auto* client = client_of(sender);
    if (!client || serial != client->ping_serial)
        return;
    client->ping_sent_at = 0;
    client->answered_at = now_microseconds();
    if (client->not_responding) {
        client->not_responding = false;
        os_log_info(server_log(), "Process %d is responding again", client->pid);
        update_cursor();
        shell_state_changed();
    }
}

WindowServerEndpoint::CursorShapeAnswer Server::cursor_shape(IPC::Sender const&)
{
    return { static_cast<i32>(m_cursor_shape), m_cursor_turn };
}

// --- Frames ---

void Server::note_input()
{
    m_input_waiting = true;
    m_last_input_at = now_microseconds();
}

void Server::schedule_frame()
{
    if (!m_compositor.has_damage() && !m_layers_animating)
        return;
    // Frames keep to a fixed beat, as a display's refresh would: a timer that fires late (they do, waking an idle processor
    // under a hypervisor) takes that time from the wait for the next, rather than slowing them all down. Unless someone's
    // waiting to see what they just did: that goes as soon as it can, at up to twice the rate. There's no refresh here to wait
    // for, and what's measured against a frame's length is how long they wait.
    u64 const now = now_microseconds();
    if (m_next_frame_time + frame_interval < now)
        m_next_frame_time = now;
    u64 const due = m_input_waiting ? max(now, m_last_frame_started + frame_interval / 2) : max(now, m_next_frame_time);
    if (m_frame_scheduled) {
        if (due >= m_frame_due)
            return;
        m_loop.cancel_timer(m_frame_timer);
    }
    m_frame_scheduled = true;
    m_frame_due = due;
    m_frame_timer = m_loop.add_timer((due - now) / 1000, false, [this] { render_frame(); });
}

void Server::render_frame()
{
    m_frame_scheduled = false;
    m_input_waiting = false;
    m_last_frame_started = now_microseconds();
    while (m_next_frame_time <= m_last_frame_started)
        m_next_frame_time += frame_interval;
    double const frame_time = static_cast<double>(m_last_frame_started) / 1'000'000.0;
    if (m_layers_animating)
        m_layers_animating = advance_layers(frame_time);
    // Title bars out of date are redrawn now, once, however many changes came.
    for (auto* window : m_windows) {
        if (window->visible && window->is_titled() && window->chrome_is_stale)
            render_chrome(*window);
    }
    if (m_gesture == Gesture::Resizing && m_gesture_window && m_compositor.has_damage()) {
        auto const* surface = m_gesture_window->shown_surface();
        int const scale = m_screen.scale();
        if (surface && (surface->width() != m_gesture_window->frame.width * scale || surface->height() != m_gesture_window->frame.height * scale))
            ++m_resize_mismatches;
    }
    m_compositor.render();
    // The application switcher's panel, there for the first time since its key: how long that took.
    if (m_switch_asked_at != 0) {
        for (auto const* window : m_windows) {
            if (!window->visible || !window->present_pending || !window->client || !window->client->is_shell || window->title.view() != "Switcher"_sv)
                continue;
            u64 const took = now_microseconds() - m_switch_asked_at;
            m_switch_asked_at = 0;
            // (On standard error, which the test sessions send to the console.)
            fprintf(stderr, "budget: Cmd-Tab appears: %llu.%llu ms\n", static_cast<unsigned long long>(took / 1000), static_cast<unsigned long long>(took % 1000 / 100));
            break;
        }
        if (!m_switching)
            m_switch_asked_at = 0;
    }
    if (m_latency_started != 0) {
        u64 const took = now_microseconds() - m_latency_started;
        m_latency_started = 0;
        m_latency_counts[min<u64>(took / 500, latency_buckets - 1)] += 1;
        m_latency_worst = max(m_latency_worst, took);
    }
    // What the clients presented is on screen: they may draw on.
    for (auto* window : m_windows) {
        if (!window->present_pending || !window->client)
            continue;
        window->present_pending = false;
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Presented));
        encoder.encode(window->id);
        send_to(*window->client, encoder);
    }
    for (auto* window : m_windows) {
        if (!window->layers_present_pending || !window->client)
            continue;
        window->layers_present_pending = false;
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::LayersPresented));
        encoder.encode(window->id);
        encoder.encode(window->layers_present_serial);
        send_to(*window->client, encoder);
    }
    log_layers(frame_time);
    if (m_recording.client)
        record_frame();
    schedule_frame();
}

// --- Clients ---

void Server::send_to(Client& client, IPC::Encoder& encoder)
{
    // Never wait for a client: if its queue is full, it misses this one.
    (void)IPC::send(client.events, encoder, true);
}

Client* Server::client_of(IPC::Sender const& sender)
{
    for (auto& client : m_clients) {
        if (client->pid == sender.pid)
            return client.ptr();
    }
    return nullptr;
}

ServerWindow* Server::window_of(IPC::Sender const& sender, u32 id)
{
    auto* client = client_of(sender);
    if (!client)
        return nullptr;
    for (auto* window : m_windows) {
        if (window->id == id && window->client == client)
            return window;
    }
    return nullptr;
}

WindowServerEndpoint::ConnectAnswer Server::connect(IPC::Sender const& sender, IPC::Port events)
{
    WindowServerEndpoint::ConnectAnswer answer {};
    auto const points = m_screen.main_display().points;
    answer.width = points.width;
    answer.height = points.height;
    answer.scale = m_screen.scale();
    answer.appearance = MUST(String::from_view(m_appearance.view()));
    if (!events.is_valid()) {
        answer.error = EINVAL;
        return answer;
    }
    if (client_of(sender)) {
        answer.error = EEXIST;
        return answer;
    }
    auto* client = new (Base::nothrow) Client;
    if (!client) {
        answer.error = ENOMEM;
        return answer;
    }
    client->pid = sender.pid;
    client->events = move(events);
    // When the client's end goes, so do its windows.
    int fd = client->events.fd();
    client->watch = m_loop.watch(fd, Core::EventLoop::Interest::Read, [this, client] { remove_client(*client); });
    m_clients.append(adopt_own(*client));
    // (Most have nothing to be told, so they're only told if there's something.)
    if (m_reduce_motion) {
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::MotionChanged));
        encoder.encode(true);
        send_to(*m_clients.last(), encoder);
    }
    return answer;
}

void Server::remove_client(Client& client)
{
    if (m_recording.client == &client)
        end_recording();
    os_log_info(server_log(), "A client is gone: %s", client.application_name.is_empty() ? "(not an application)" : client.application_name.characters());
    for (size_t i = m_windows.size(); i > 0; --i) {
        if (m_windows[i - 1]->client == &client)
            remove_window(*m_windows[i - 1]);
    }
    m_loop.unwatch(client.watch);
    m_hotkeys.remove_all_matching([&](auto const& hotkey) { return hotkey.client == &client; });
    if (m_active_application == &client)
        m_active_application = nullptr;
    m_recent_applications.remove_all_matching([&](auto* one) { return one == &client; });
    m_stage_strip.remove_all_matching([&](auto const& item) { return item.client == &client; });
    if (m_stage_shown == &client)
        m_stage_shown = nullptr;
    if (m_stage) {
        layout_stage_strip();
        refresh_lasting_overlays();
    }
    m_clients.remove_all_matching([&](auto const& owned) { return owned.ptr() == &client; });
    shell_state_changed();
    schedule_frame();
}

void Server::release_surface(ServerWindow& window)
{
    window.buffers[0] = nullptr;
    window.buffers[1] = nullptr;
    if (window.memory)
        munmap(window.memory, window.memory_size);
    window.memory = nullptr;
    window.memory_size = 0;
    window.has_presented = false;
}

void Server::remove_window(ServerWindow& window)
{
    if (window.full_screen)
        leave_full_screen(window, true);
    m_compositor.invalidate_window(window);
    release_surface(window);
    if (window.waiting_memory)
        munmap(window.waiting_memory, window.waiting_memory_size);
    window.waiting_memory = nullptr;
    window.waiting_buffers[0] = nullptr;
    window.waiting_buffers[1] = nullptr;
    if (m_active == &window)
        m_active = nullptr;
    if (m_hovered == &window)
        m_hovered = nullptr;
    if (m_gesture_window == &window) {
        m_gesture_window = nullptr;
        m_gesture = Gesture::None;
    }
    m_tile_neighbors.remove_all_matching([&](auto const& neighbor) { return neighbor.window == &window; });
    if (m_window_menu_window == &window)
        disarm_window_menu();
    if (m_grab == &window)
        m_grab = nullptr;
    if (m_drag) {
        if (m_drag->source == &window || m_drag->image == &window)
            m_drag = nullptr;
        else if (m_drag->target == &window)
            m_drag->target = nullptr;
    }
    m_genies.remove_all_matching([&](auto const& genie) { return genie.window == &window; });
    m_thumbnails.remove_all_matching([&](auto const& thumbnail) { return thumbnail.window == &window; });
    if (m_overview_chosen == &window)
        m_overview_chosen = nullptr;
    m_windows.remove_all_matching([&](auto* other) { return other == &window; });
    delete &window;
    // The next window down takes over.
    if (!m_active)
        activate_frontmost();
    shell_state_changed();
}

void Server::activate_frontmost()
{
    for (size_t i = m_windows.size(); i > 0; --i) {
        auto* window = m_windows[i - 1];
        if (window->visible && window->can_become_key()) {
            activate(window);
            return;
        }
    }
}

WindowServerEndpoint::CreateWindowAnswer Server::create_window(IPC::Sender const& sender, String title, i32 x, i32 y, i32 width, i32 height, u32 flags)
{
    os_log_debug(server_log(), "Process %d creates \"%s\": %dx%d at %d,%d, flags %#x", sender.pid, title.characters(), width, height, x, y, flags);
    WindowServerEndpoint::CreateWindowAnswer answer {};
    auto* client = client_of(sender);
    if (!client || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        answer.error = client ? EINVAL : ENOTCONN;
        return answer;
    }
    auto* window = new (Base::nothrow) ServerWindow;
    if (!window) {
        answer.error = ENOMEM;
        return answer;
    }
    window->id = m_next_window_id++;
    window->client = client;
    window->title = move(title);
    window->frame = { x, y, width, height };
    window->flags = flags;
    window->shape = m_compositor.default_shape();
    window->level = (flags & Window::WindowPopup) ? Window::LevelPopup : Window::LevelNormal;
    window->space = space_for_new_window(*window);
    insert_by_level(*window);
    answer.window = window->id;
    if (!(flags & Window::WindowHidden)) {
        window->shown_by_client = true;
        update_visibility(*window);
        if (window->can_become_key())
            activate(window);
        m_compositor.invalidate_window(*window);
    }
    shell_state_changed();
    schedule_frame();
    return answer;
}

void Server::destroy_window(IPC::Sender const& sender, u32 id)
{
    os_log_debug(server_log(), "Process %d destroys window %u", sender.pid, id);
    if (auto* window = window_of(sender, id)) {
        remove_window(*window);
        schedule_frame();
    }
}

void Server::set_title(IPC::Sender const& sender, u32 id, String title)
{
    auto* window = window_of(sender, id);
    if (!window)
        return;
    window->title = move(title);
    invalidate_title_bar(*window);
}

void Server::set_frame(IPC::Sender const& sender, u32 id, i32 x, i32 y, i32 width, i32 height)
{
    auto* window = window_of(sender, id);
    if (!window || width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return;
    move_window_to(*window, { x, y, width, height });
}

void Server::activate(IPC::Sender const& sender, u32 id)
{
    auto* window = window_of(sender, id);
    if (window && window->visible && window->can_become_key())
        activate(window);
}

void Server::set_level(IPC::Sender const& sender, u32 id, i32 level)
{
    auto* window = window_of(sender, id);
    if (!window || window->level == level)
        return;
    window->level = level;
    m_windows.remove_all_matching([&](auto* other) { return other == window; });
    insert_by_level(*window);
    m_compositor.invalidate_window(*window);
    schedule_frame();
}

void Server::set_corner_radius(IPC::Sender const& sender, u32 id, i32 radius)
{
    auto* window = window_of(sender, id);
    if (!window || radius > 64)
        return;
    auto shape = radius < 0 ? ErrorOr<WindowShape const*>(m_compositor.default_shape()) : m_compositor.shape_with_radius(radius);
    if (shape.is_error())
        return;
    m_compositor.invalidate_window(*window);
    window->shape = shape.value();
    m_compositor.invalidate_window(*window);
    schedule_frame();
}

void Server::set_title_bar(IPC::Sender const& sender, u32 id, i32 height)
{
    auto* window = window_of(sender, id);
    if (!window || height < 0 || height > 512)
        return;
    // Over the old height as well as the new.
    invalidate_title_bar(*window);
    window->title_bar_override = height;
    invalidate_title_bar(*window);
}

void Server::begin_move(IPC::Sender const& sender, u32 id)
{
    auto* window = window_of(sender, id);
    if (window && m_gesture == Gesture::ContentPress && m_gesture_window == window)
        m_gesture = Gesture::Moving;
}

void Server::grab_pointer(IPC::Sender const& sender, u32 id, bool grab)
{
    auto* window = window_of(sender, id);
    if (!window)
        return;
    if (!grab) {
        if (m_grab == window)
            m_grab = nullptr;
        return;
    }
    m_grab = window;
    m_gesture = Gesture::None;
    m_gesture_window = nullptr;
}

void Server::set_cursor(IPC::Sender const& sender, u32 id, i32 shape)
{
    auto* window = window_of(sender, id);
    if (!window || shape < 0 || shape >= static_cast<i32>(Window::CursorShape::Count))
        return;
    window->cursor = static_cast<Window::CursorShape>(shape);
    update_cursor();
}

i32 Server::set_keyboard_layout(IPC::Sender const&, String identifier)
{
    auto layout = KeyboardLayout::load(identifier.view());
    if (layout.is_error())
        return ENOENT;
    m_keyboard_layout = layout.release_value();
    return 0;
}

WindowServerEndpoint::DisplaysAnswer Server::displays(IPC::Sender const&)
{
    WindowServerEndpoint::DisplaysAnswer answer {};
    auto const& displays = m_screen.displays();
    MUST(answer.records.try_resize(displays.size() * sizeof(Window::DisplayRecord)));
    for (size_t i = 0; i < displays.size(); ++i) {
        auto const& display = displays[i];
        Window::DisplayRecord const record { display.output, display.points.x, display.points.y, display.points.width, display.points.height, display.panel.width, display.panel.height };
        __builtin_memcpy(answer.records.data() + i * sizeof(record), &record, sizeof(record));
    }
    answer.mirrored = m_screen.is_mirrored();
    answer.outputs = static_cast<i32>(m_screen.output_count());
    return answer;
}

IntRect Server::work_area_of(Display const& display) const
{
    if (&display == &m_screen.main_display())
        return { display.points.x + m_work_left, display.points.y + m_work_top, display.points.width - m_work_left - m_work_right, display.points.height - m_work_top - m_work_bottom };
    // Each has a menu bar. Only the main one has the Dock.
    return { display.points.x, display.points.y + m_work_top, display.points.width, display.points.height - m_work_top };
}

void Server::displays_changed()
{
    sync_display_spaces();
    MUST(m_compositor.screen_changed());
    // What's now off every display comes back on to the nearest.
    for (auto* window : m_windows) {
        if (window->level != Window::LevelNormal)
            continue;
        auto const center = window->frame.center();
        auto const& display = m_screen.display_at(center);
        if (!display.points.contains(center)) {
            auto moved = window->frame;
            auto const area = work_area_of(display);
            moved.x = clamp(moved.x, area.x, max(area.x, area.right() - moved.width));
            moved.y = clamp(moved.y, area.y, max(area.y, area.bottom() - moved.height));
            move_window_to(*window, moved);
            send_moved(*window);
        }
    }
    auto const main = m_screen.main_display().points;
    for (auto& client : m_clients) {
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::DisplaysChanged));
        encoder.encode(static_cast<i32>(main.width));
        encoder.encode(static_cast<i32>(main.height));
        send_to(*client, encoder);
    }
    m_pointer = m_screen.nearest_point_on_a_display(m_pointer);
    set_up_cursor();
    shell_state_changed();
    schedule_frame();
}

i32 Server::set_display(IPC::Sender const&, i32 output, i32 x, i32 y, i32 width, i32 height)
{
    Screen::Placement placements[8];
    size_t count = 0;
    // The others stay where they were, for now: all of them have to be said.
    for (auto const& display : m_screen.displays()) {
        if (count == 8)
            break;
        placements[count++] = Screen::Placement { display.output, display.points.origin(), display.points.size() };
    }
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (placements[i].output == output) {
            placements[i].origin = { x, y };
            placements[i].points = { width, height };
            found = true;
        }
    }
    if (!found)
        return ENOENT;
    if (auto arranged = m_screen.arrange({ placements, count }, m_screen.is_mirrored()); arranged.is_error())
        return arranged.error().code();
    displays_changed();
    return 0;
}

i32 Server::set_mirroring(IPC::Sender const&, bool mirrored)
{
    if (mirrored == m_screen.is_mirrored())
        return 0;
    if (auto arranged = m_screen.arrange({}, mirrored); arranged.is_error())
        return arranged.error().code();
    displays_changed();
    return 0;
}

i32 Server::set_warmth(IPC::Sender const&, i32 percent)
{
    if (auto changed = m_screen.set_warmth(static_cast<float>(clamp(percent, 0, 100)) / 100); changed.is_error())
        return changed.error().code();
    // What's on the screen is shown through it, and may have moved to memory of its own.
    MUST(m_compositor.screen_changed());
    schedule_frame();
    return 0;
}

void Server::set_reduce_motion(IPC::Sender const&, bool reduced)
{
    if (reduced == m_reduce_motion)
        return;
    m_reduce_motion = reduced;
    for (auto& client : m_clients) {
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::MotionChanged));
        encoder.encode(reduced);
        send_to(*client, encoder);
    }
}

i32 Server::set_appearance(IPC::Sender const&, String name)
{
    char const* path = nullptr;
    if (name.view() == "Light"_sv)
        path = "/System/Library/Themes/Light.json";
    else if (name.view() == "Dark"_sv)
        path = "/System/Library/Themes/Dark.json";
    else
        return EINVAL;
    if (m_appearance.view() == name.view())
        return 0;
    auto theme = load_theme(StringView(path));
    if (theme.is_error())
        return theme.error().is_errno() ? theme.error().code() : EINVAL;
    m_theme = theme.release_value();
    m_appearance = move(name);
    if (m_compositor.theme_changed().is_error())
        return ENOMEM;
    for (auto* window : m_windows)
        window->chrome_is_stale = true;
    apply_wallpaper();
    for (auto& client : m_clients) {
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::AppearanceChanged));
        encoder.encode(m_appearance.view());
        send_to(*client, encoder);
    }
    schedule_frame();
    return 0;
}

void Server::begin_drag(IPC::Sender const& sender, u32 id, u32 image_id, String type, Vector<u8> data)
{
    auto* window = window_of(sender, id);
    auto* image = window_of(sender, image_id);
    if (!window || !image || m_drag)
        return;
    bool pressed_here = (m_gesture == Gesture::ContentPress && m_gesture_window == window) || m_grab == window;
    if (!pressed_here)
        return;
    auto* drag = new (Base::nothrow) Drag;
    if (!drag)
        return;
    drag->source = window;
    drag->image = image;
    drag->image_offset = { image->frame.x - m_pointer.x, image->frame.y - m_pointer.y };
    drag->type = move(type);
    drag->data = move(data);
    m_drag = adopt_own(*drag);
    m_gesture = Gesture::None;
    m_gesture_window = nullptr;
    if (m_grab == window)
        m_grab = nullptr;
    image->level = Window::LevelDragImage;
    m_windows.remove_all_matching([&](auto* other) { return other == image; });
    insert_by_level(*image);
    update_cursor();
}

void Server::accept_drop(IPC::Sender const& sender, u32 id, bool accepted)
{
    if (auto* window = window_of(sender, id))
        window->accepts_drop = accepted;
}

void Server::show(IPC::Sender const& sender, u32 id, bool visible)
{
    auto* window = window_of(sender, id);
    if (!window || window->shown_by_client == visible)
        return;
    window->shown_by_client = visible;
    window->minimized = false;
    if (visible && !window->is_on_every_space() && !window->full_screen)
        window->space = space_for_new_window(*window);
    if (visible)
        reveal_desktop(false, true);
    update_visibility(*window);
    // A new window on a display that's showing someone's full screen: over to it.
    if (visible && window->can_become_key())
        show_space_of(*window);
    m_compositor.invalidate_window(*window);
    if (visible) {
        // The session's first window ends the boot screen.
        if (m_boot_screen && window->can_become_key())
            m_boot_finishing = true;
        if (window->can_become_key())
            activate(window);
        else
            raise(*window);
    }
    shell_state_changed();
    schedule_frame();
}

i32 Server::set_surface(IPC::Sender const& sender, u32 id, IPC::Port memory, i32 width, i32 height)
{
    auto* window = window_of(sender, id);
    if (!window)
        return ENOENT;
    if (!memory.is_valid() || width <= 0 || height <= 0 || width > 32768 || height > 32768)
        return EINVAL;
    size_t buffer_size = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(Pixel);
    size_t total = buffer_size * 2;
    // A file that small would fault here, not in the client.
    struct stat information {};
    if (fstat(memory.fd(), &information) < 0 || static_cast<size_t>(information.st_size) < total)
        return EINVAL;
    void* mapped = mmap(nullptr, total, PROT_READ, MAP_SHARED, memory.fd(), 0);
    if (mapped == MAP_FAILED)
        return errno;
    auto* first = static_cast<Pixel*>(mapped);
    auto one = Bitmap::wrap(first, { width, height }, width);
    auto two = Bitmap::wrap(first + static_cast<size_t>(width) * static_cast<size_t>(height), { width, height }, width);
    if (one.is_error() || two.is_error()) {
        munmap(mapped, total);
        return ENOMEM;
    }
    // The first one, or one that hasn't been shown: straight in. Otherwise it waits, with what's on the screen staying there, until
    // it has something on it, which is what stops a resized window showing nothing for a frame.
    if (window->waiting_memory)
        munmap(window->waiting_memory, window->waiting_memory_size);
    window->waiting_memory = nullptr;
    window->waiting_buffers[0] = nullptr;
    window->waiting_buffers[1] = nullptr;
    if (window->has_presented) {
        window->waiting_memory = mapped;
        window->waiting_memory_size = total;
        window->waiting_buffers[0] = one.release_value();
        window->waiting_buffers[1] = two.release_value();
        return 0;
    }
    release_surface(*window);
    window->memory = mapped;
    window->memory_size = total;
    window->buffers[0] = one.release_value();
    window->buffers[1] = two.release_value();
    window->shown_buffer = 0;
    m_compositor.surface_replaced(*window);
    return 0;
}

void Server::show_waiting_surface(ServerWindow& window)
{
    if (!window.waiting_memory)
        return;
    release_surface(window);
    window.memory = window.waiting_memory;
    window.memory_size = window.waiting_memory_size;
    window.buffers[0] = move(window.waiting_buffers[0]);
    window.buffers[1] = move(window.waiting_buffers[1]);
    window.waiting_memory = nullptr;
    window.waiting_memory_size = 0;
    window.shown_buffer = 0;
    m_compositor.surface_replaced(window);
}

void Server::present(IPC::Sender const& sender, u32 id, u32 buffer, i32 x, i32 y, i32 width, i32 height)
{
    auto* window = window_of(sender, id);
    if (!window || buffer > 1)
        return;
    show_waiting_surface(*window);
    if (!window->buffers[0])
        return;
    window->shown_buffer = static_cast<int>(buffer);
    window->has_presented = true;
    window->present_pending = true;
    m_compositor.surface_changed(*window, { x, y, width, height });
    // What someone just did, as like as not.
    if (now_microseconds() - m_last_input_at < 2 * frame_interval)
        m_input_waiting = true;
    if (window->visible) {
        auto frame = m_compositor.device_frame(*window);
        // (The first frame of a surface is all new.)
        IntRect damage { frame.x + x, frame.y + y, width, height };
        m_compositor.invalidate(damage.intersected(frame));
    }
    // Something to answer for, even with nothing to draw (a hidden window).
    if (!m_compositor.has_damage())
        m_compositor.invalidate({ 0, 0, 1, 1 });
    finish_live_resize(*window);
    schedule_frame();
}

// ---- Layers

void Layer::release_surface()
{
    buffers[0] = nullptr;
    buffers[1] = nullptr;
    if (memory)
        munmap(memory, memory_size);
    memory = nullptr;
    memory_size = 0;
    has_presented = false;
}

namespace {

// How many values a property has, where they are, and how to set them.
int component_count(Window::LayerProperty property)
{
    using Window::LayerProperty;
    switch (property) {
    case LayerProperty::Frame:
        return 4;
    case LayerProperty::Translation:
    case LayerProperty::Scale:
    case LayerProperty::BoundsOrigin:
        return 2;
    case LayerProperty::Rotation:
    case LayerProperty::Opacity:
    case LayerProperty::CornerRadius:
        return 1;
    default:
        return 0;
    }
}

void read_property(LayerState const& state, Window::LayerProperty property, float* out)
{
    using Window::LayerProperty;
    switch (property) {
    case LayerProperty::Frame:
        out[0] = state.frame.x;
        out[1] = state.frame.y;
        out[2] = state.frame.width;
        out[3] = state.frame.height;
        break;
    case LayerProperty::Translation:
        out[0] = state.translation.x;
        out[1] = state.translation.y;
        break;
    case LayerProperty::Scale:
        out[0] = state.scale.x;
        out[1] = state.scale.y;
        break;
    case LayerProperty::BoundsOrigin:
        out[0] = state.bounds_origin.x;
        out[1] = state.bounds_origin.y;
        break;
    case LayerProperty::Rotation:
        out[0] = state.rotation;
        break;
    case LayerProperty::Opacity:
        out[0] = state.opacity;
        break;
    case LayerProperty::CornerRadius:
        out[0] = state.corner_radius;
        break;
    default:
        break;
    }
}

void write_property(LayerState& state, Window::LayerProperty property, float const* in)
{
    using Window::LayerProperty;
    switch (property) {
    case LayerProperty::Frame:
        state.frame = { in[0], in[1], in[2], in[3] };
        break;
    case LayerProperty::Translation:
        state.translation = { in[0], in[1] };
        break;
    case LayerProperty::Scale:
        state.scale = { in[0], in[1] };
        break;
    case LayerProperty::BoundsOrigin:
        state.bounds_origin = { in[0], in[1] };
        break;
    case LayerProperty::Rotation:
        state.rotation = in[0];
        break;
    case LayerProperty::Opacity:
        state.opacity = in[0];
        break;
    case LayerProperty::CornerRadius:
        state.corner_radius = in[0];
        break;
    default:
        break;
    }
}

}

WindowServerEndpoint::CreateLayerAnswer Server::create_layer(IPC::Sender const& sender, u32 id, u32 parent_id)
{
    WindowServerEndpoint::CreateLayerAnswer answer {};
    auto* window = window_of(sender, id);
    if (!window) {
        answer.error = ENOENT;
        return answer;
    }
    Layer* parent = nullptr;
    if (parent_id != 0) {
        parent = window->layer_with_id(parent_id);
        if (!parent) {
            answer.error = ENOENT;
            return answer;
        }
    }
    if (window->layers.size() >= 4096) {
        answer.error = ENOSPC;
        return answer;
    }
    auto layer = adopt_own(*new Layer);
    layer->id = window->next_layer_id++;
    layer->parent = parent;
    auto* raw = layer.ptr();
    window->layers.append(move(layer));
    if (parent)
        parent->children.append(raw);
    else
        window->top_layers.append(raw);
    answer.layer = raw->id;
    return answer;
}

i32 Server::set_layer_surface(IPC::Sender const& sender, u32 id, u32 layer_id, IPC::Port memory, i32 width, i32 height)
{
    auto* window = window_of(sender, id);
    auto* layer = window ? window->layer_with_id(layer_id) : nullptr;
    if (!layer)
        return ENOENT;
    if (!memory.is_valid() || width <= 0 || height <= 0 || width > 32768 || height > 32768)
        return EINVAL;
    size_t const buffer_size = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(Pixel);
    size_t const total = buffer_size * 2;
    // A file that small would fault here, not in the client.
    struct stat information {};
    if (fstat(memory.fd(), &information) < 0 || static_cast<size_t>(information.st_size) < total)
        return EINVAL;
    void* mapped = mmap(nullptr, total, PROT_READ, MAP_SHARED, memory.fd(), 0);
    if (mapped == MAP_FAILED)
        return errno;
    auto* first = static_cast<Pixel*>(mapped);
    auto one = Bitmap::wrap(first, { width, height }, width);
    auto two = Bitmap::wrap(first + static_cast<size_t>(width) * static_cast<size_t>(height), { width, height }, width);
    if (one.is_error() || two.is_error()) {
        munmap(mapped, total);
        return ENOMEM;
    }
    m_compositor.invalidate_layer(*window, *layer);
    layer->release_surface();
    layer->memory = mapped;
    layer->memory_size = total;
    layer->buffers[0] = one.release_value();
    layer->buffers[1] = two.release_value();
    layer->shown_buffer = 0;
    schedule_frame();
    return 0;
}

void Server::destroy_layer(ServerWindow& window, Layer& layer)
{
    m_compositor.invalidate_layer(window, layer);
    // The innermost first, as each takes itself out of the list it's in.
    while (!layer.children.is_empty())
        destroy_layer(window, *layer.children.last());
    if (layer.parent)
        layer.parent->children.remove_all_matching([&](auto* other) { return other == &layer; });
    else
        window.top_layers.remove_all_matching([&](auto* other) { return other == &layer; });
    window.layers.remove_all_matching([&](auto const& other) { return other.ptr() == &layer; });
}

void Server::apply_layer_change(ServerWindow& window, Window::LayerChange const& change, double now)
{
    using Window::LayerProperty;
    if (change.property >= LayerProperty::Count)
        return;
    if (change.property == LayerProperty::PresentWindow) {
        show_waiting_surface(window);
        if (!window.buffers[0])
            return;
        window.shown_buffer = change.values[0] != 0 ? 1 : 0;
        window.has_presented = true;
        IntRect const damage { static_cast<int>(change.values[1]), static_cast<int>(change.values[2]), static_cast<int>(change.values[3]), static_cast<int>(change.values[4]) };
        m_compositor.surface_changed(window, damage);
        if (window.visible) {
            auto const frame = m_compositor.device_frame(window);
            m_compositor.invalidate(IntRect { frame.x + damage.x, frame.y + damage.y, damage.width, damage.height }.intersected(frame));
        }
        finish_live_resize(window);
        return;
    }
    auto* layer = window.layer_with_id(change.layer);
    if (!layer)
        return;
    auto const& v = change.values;
    m_compositor.invalidate_layer(window, *layer);
    switch (change.property) {
    case LayerProperty::Frame:
    case LayerProperty::Translation:
    case LayerProperty::Scale:
    case LayerProperty::Rotation:
    case LayerProperty::Opacity:
    case LayerProperty::CornerRadius:
    case LayerProperty::BoundsOrigin:
        // Set outright, it's no longer on its way anywhere.
        layer->animations.remove_all_matching([&](auto const& animation) { return animation.property == change.property; });
        write_property(layer->model, change.property, v);
        write_property(layer->shown, change.property, v);
        break;
    case LayerProperty::ContentsOrigin:
        layer->contents_origin = { v[0], v[1] };
        break;
    case LayerProperty::Background:
        layer->background = Color { static_cast<u8>(clamp(v[0], 0.0f, 1.0f) * 255 + 0.5f), static_cast<u8>(clamp(v[1], 0.0f, 1.0f) * 255 + 0.5f), static_cast<u8>(clamp(v[2], 0.0f, 1.0f) * 255 + 0.5f), static_cast<u8>(clamp(v[3], 0.0f, 1.0f) * 255 + 0.5f) };
        break;
    case LayerProperty::Hidden:
        layer->hidden = v[0] != 0;
        break;
    case LayerProperty::Clips:
        layer->clips = v[0] != 0;
        break;
    case LayerProperty::ClipRect:
        layer->has_clip_rect = v[2] >= 0;
        layer->clip_rect = { v[0], v[1], v[2], v[3] };
        break;
    case LayerProperty::Material: {
        int const material = static_cast<int>(v[0]);
        layer->material = material >= 0 && material < static_cast<int>(Window::Material::Count) ? material : -1;
        break;
    }
    case LayerProperty::Blur:
        layer->blur = max(v[0], 0.0f);
        break;
    case LayerProperty::Logged:
        layer->logged = v[0] != 0;
        layer->log.clear();
        break;
    case LayerProperty::Present: {
        if (!layer->buffers[0])
            break;
        layer->shown_buffer = v[0] != 0 ? 1 : 0;
        layer->has_presented = true;
        break;
    }
    case LayerProperty::Order: {
        auto& list = layer->parent ? layer->parent->children : window.top_layers;
        list.remove_all_matching([&](auto* other) { return other == layer; });
        size_t const place = min(static_cast<size_t>(max(v[0], 0.0f)), list.size());
        MUST(list.try_insert(place, layer));
        break;
    }
    case LayerProperty::Animate: {
        auto const property = static_cast<LayerProperty>(change.detail & 0xff);
        int const components = component_count(property);
        if (components == 0)
            break;
        LayerAnimation animation;
        animation.property = property;
        for (int i = 0; i < 4; ++i) {
            animation.from[i] = v[i];
            animation.to[i] = v[4 + i];
        }
        animation.timing.duration = v[8];
        switch (static_cast<Window::LayerCurve>((change.detail >> 8) & 0xff)) {
        case Window::LayerCurve::Linear:
            animation.timing.kind = Animation::Timing::Kind::Linear;
            break;
        case Window::LayerCurve::Bezier:
            animation.timing.kind = Animation::Timing::Kind::Bezier;
            animation.timing.bezier = Animation::CubicBezier(v[9], v[10], v[11], v[12]);
            break;
        case Window::LayerCurve::Spring:
            animation.timing.kind = Animation::Timing::Kind::Spring;
            animation.timing.spring = Animation::Spring::with_frequency(v[9], v[10], v[11]);
            break;
        }
        animation.started = now;
        animation.repeats = change.detail & (1u << 16);
        animation.steps = static_cast<int>(change.detail >> 24);
        animation.tag = change.tag;
        // Only one at a time of any one thing.
        layer->animations.remove_all_matching([&](auto const& other) { return other.property == property; });
        // It ends where it says, and it's there as far as anything's concerned, from now on.
        write_property(layer->model, property, animation.to);
        write_property(layer->shown, property, animation.from);
        layer->animations.append(move(animation));
        m_layers_animating = true;
        break;
    }
    case LayerProperty::StopAnimation: {
        auto const property = static_cast<LayerProperty>(change.detail & 0xff);
        float where[4] {};
        read_property(layer->shown, property, where);
        write_property(layer->model, property, where);
        layer->animations.remove_all_matching([&](auto const& animation) { return animation.property == property; });
        break;
    }
    case LayerProperty::Destroy:
        destroy_layer(window, *layer);
        return;
    default:
        break;
    }
    m_compositor.invalidate_layer(window, *layer);
    if (change.property == LayerProperty::Present) {
        IntRect const damage { static_cast<int>(v[1]), static_cast<int>(v[2]), static_cast<int>(v[3]), static_cast<int>(v[4]) };
        if (window.visible)
            m_compositor.invalidate(m_compositor.device_rect_of_layer_surface(window, *layer, damage));
    }
}

void Server::commit_layers(IPC::Sender const& sender, u32 id, u32 serial, Vector<u8> changes)
{
    auto* window = window_of(sender, id);
    if (!window || changes.size() % sizeof(Window::LayerChange) != 0)
        return;
    double const now = static_cast<double>(now_microseconds()) / 1'000'000.0;
    for (size_t at = 0; at < changes.size(); at += sizeof(Window::LayerChange)) {
        Window::LayerChange change;
        __builtin_memcpy(&change, changes.data() + at, sizeof(change));
        apply_layer_change(*window, change, now);
    }
    window->layers_present_pending = true;
    window->layers_present_serial = serial;
    // Something to answer for, even with nothing to draw.
    if (!m_compositor.has_damage())
        m_compositor.invalidate({ 0, 0, 1, 1 });
    if (now_microseconds() - m_last_input_at < 2 * frame_interval)
        m_input_waiting = true;
    schedule_frame();
}

bool Server::advance_layers(double now)
{
    using Window::LayerProperty;
    bool still_going = false;
    struct Finished {
        ServerWindow* window;
        u32 layer;
        u32 tag;
    };
    Vector<Finished> finished;
    for (auto* window : m_windows) {
        for (auto& layer : window->layers) {
            if (layer->animations.is_empty())
                continue;
            m_compositor.invalidate_layer(*window, *layer);
            for (size_t i = 0; i < layer->animations.size();) {
                auto& animation = layer->animations[i];
                auto elapsed = static_cast<float>(now - animation.started);
                float const duration = animation.timing.duration;
                bool const done = !animation.repeats && elapsed >= duration;
                if (animation.repeats && duration > 0) {
                    while (elapsed >= duration)
                        elapsed -= duration;
                }
                float progress = done ? 1 : animation.timing.value_at(elapsed);
                if (animation.steps > 0)
                    progress = static_cast<float>(static_cast<int>(progress * static_cast<float>(animation.steps))) / static_cast<float>(animation.steps);
                float values[4];
                for (int c = 0; c < 4; ++c)
                    values[c] = animation.from[c] + (animation.to[c] - animation.from[c]) * progress;
                write_property(layer->shown, animation.property, done ? animation.to : values);
                if (done) {
                    if (animation.tag)
                        finished.append(Finished { window, layer->id, animation.tag });
                    layer->animations.remove(i);
                } else {
                    still_going = true;
                    ++i;
                }
            }
            m_compositor.invalidate_layer(*window, *layer);
        }
    }
    for (auto const& ended : finished) {
        if (!ended.window->client)
            continue;
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::LayerAnimationFinished));
        encoder.encode(ended.window->id);
        encoder.encode(ended.layer);
        encoder.encode(ended.tag);
        send_to(*ended.window->client, encoder);
    }
    return still_going;
}

void Server::log_layers(double now)
{
    for (auto* window : m_windows) {
        for (auto& layer : window->layers) {
            if (!layer->logged || layer->animations.is_empty() || layer->log.size() >= 8192)
                continue;
            auto const& animation = layer->animations[0];
            LayerLogEntry entry;
            entry.seconds = now - animation.started;
            read_property(layer->shown, animation.property, entry.values);
            layer->log.append(entry);
        }
    }
}

WindowServerEndpoint::LayerFrameLogAnswer Server::layer_frame_log(IPC::Sender const& sender, u32 id, u32 layer_id)
{
    WindowServerEndpoint::LayerFrameLogAnswer answer {};
    if (sender.uid != 0) {
        answer.error = EPERM;
        return answer;
    }
    for (auto* window : m_windows) {
        if (window->id != id)
            continue;
        auto* layer = window->layer_with_id(layer_id);
        if (!layer) {
            answer.error = ENOENT;
            return answer;
        }
        MUST(answer.log.try_resize(layer->log.size() * sizeof(LayerLogEntry)));
        if (!layer->log.is_empty())
            __builtin_memcpy(answer.log.data(), layer->log.data(), answer.log.size());
        layer->log.clear();
        return answer;
    }
    answer.error = ENOENT;
    return answer;
}

void Server::set_materials(IPC::Sender const& sender, u32 id, Vector<i32> regions)
{
    auto* window = window_of(sender, id);
    if (!window || regions.size() % 6 != 0)
        return;
    if (window->visible)
        m_compositor.invalidate_window(*window);
    window->materials.clear();
    for (size_t i = 0; i < regions.size(); i += 6) {
        i32 material = regions[i + 4];
        if (material < 0 || material >= static_cast<i32>(Material::Count))
            continue;
        window->materials.append(MaterialRegion { { regions[i], regions[i + 1], regions[i + 2], regions[i + 3] }, static_cast<Material>(material), max(regions[i + 5], 0) });
    }
    if (window->visible)
        m_compositor.invalidate_window(*window);
    schedule_frame();
}

void Server::inject_pointer(IPC::Sender const& sender, i32 x, i32 y, u32 buttons, u64 sent_at)
{
    if (sender.uid != 0)
        return;
    m_event_time = sent_at;
    pointer_changed({ x, y }, buttons);
    m_event_time = 0;
}

WindowServerEndpoint::StatisticsAnswer Server::statistics(IPC::Sender const&)
{
    return { m_compositor.frames(), m_compositor.compose_microseconds(), m_compositor.flush_microseconds(), m_compositor.pixels(), m_compositor.material_microseconds(), m_resize_mismatches };
}

void Server::damage_everything(IPC::Sender const& sender)
{
    if (sender.uid != 0)
        return;
    m_compositor.invalidate_all();
    schedule_frame();
}

WindowServerEndpoint::InputLatencyAnswer Server::input_latency(IPC::Sender const&)
{
    WindowServerEndpoint::InputLatencyAnswer answer {};
    for (auto count : m_latency_counts)
        answer.count += count;
    u64 const at_percentile = answer.count - answer.count / 100;
    u64 seen = 0;
    for (size_t i = 0; i < latency_buckets; ++i) {
        seen += m_latency_counts[i];
        if (seen >= at_percentile && answer.count != 0) {
            answer.p99_microseconds = (i + 1) * 500;
            break;
        }
    }
    answer.worst_microseconds = m_latency_worst;
    __builtin_memset(m_latency_counts, 0, sizeof(m_latency_counts));
    m_latency_worst = 0;
    return answer;
}

bool Server::may_look_at_screen(IPC::Sender const& sender, char const* doing_what)
{
    if (sender.uid == 0)
        return true;
    // The kernel knows what it's running. What came with the system has no need to ask, as long as it's the logged in person's;
    // anything else needs to have been told yes.
    int allowed = 0;
    size_t length = sizeof(allowed);
    int pid = sender.pid;
    bool const known = sysctlbyname("security.privacy.screen", &allowed, &length, &pid, sizeof(pid)) == 0;
    if (known && allowed && m_has_shell_uid && sender.uid == m_shell_uid)
        return true;
    os_log(server_log(), "Process %d, of user %d, was refused %s: nobody has said it may", sender.pid, sender.uid, doing_what);
    printf("WindowServer: process %d was refused %s\n", sender.pid, doing_what);
    fflush(stdout);
    return false;
}

ErrorOr<Vector<u8>> Server::png_of_area(IntRect points)
{
    int const scale = m_screen.scale();
    auto const origin = m_screen.origin();
    IntRect const pixels = IntRect { (points.x + origin.x) * scale, (points.y + origin.y) * scale, points.width * scale, points.height * scale }.intersected(m_screen.framebuffer().rect());
    if (pixels.is_empty())
        return Error::from_errno(EINVAL);
    auto part = TRY(Bitmap::create(pixels.size()));
    for (int y = 0; y < pixels.height; ++y)
        __builtin_memcpy(part->scanline(y), m_screen.framebuffer().scanline(pixels.y + y) + pixels.x, static_cast<size_t>(pixels.width) * sizeof(Pixel));
    return Image::encode_png(*part);
}

WindowServerEndpoint::CaptureAnswer Server::capture(IPC::Sender const& sender)
{
    WindowServerEndpoint::CaptureAnswer answer {};
    if (!may_look_at_screen(sender, "a picture of the screen")) {
        answer.error = EPERM;
        return answer;
    }
    auto encoded = Image::encode_png(m_screen.framebuffer());
    if (encoded.is_error()) {
        answer.error = ENOMEM;
        return answer;
    }
    answer.png = encoded.release_value();
    return answer;
}

WindowServerEndpoint::CaptureAreaAnswer Server::capture_area(IPC::Sender const& sender, i32 x, i32 y, i32 width, i32 height)
{
    WindowServerEndpoint::CaptureAreaAnswer answer {};
    if (!may_look_at_screen(sender, "a picture of the screen")) {
        answer.error = EPERM;
        return answer;
    }
    auto encoded = png_of_area({ x, y, width, height });
    if (encoded.is_error()) {
        answer.error = encoded.error().is_errno() ? encoded.error().code() : ENOMEM;
        return answer;
    }
    answer.png = encoded.release_value();
    return answer;
}

i32 Server::start_recording(IPC::Sender const& sender, IPC::Port memory, i32 x, i32 y, i32 width, i32 height)
{
    if (!may_look_at_screen(sender, "a recording of the screen"))
        return EPERM;
    if (m_recording.client)
        return EBUSY;
    auto* client = client_of(sender);
    if (!client || !memory.is_valid() || width <= 0 || height <= 0)
        return EINVAL;
    int const scale = m_screen.scale();
    size_t const size = static_cast<size_t>(width * scale) * static_cast<size_t>(height * scale) * sizeof(Pixel);
    struct stat information {};
    if (fstat(memory.fd(), &information) < 0 || static_cast<size_t>(information.st_size) < size)
        return EINVAL;
    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, memory.fd(), 0);
    if (mapped == MAP_FAILED)
        return errno;
    m_recording = { client, mapped, size, { x, y, width, height }, true, true };
    m_compositor.invalidate_all();
    schedule_frame();
    os_log_info(server_log(), "Process %d is recording the screen", sender.pid);
    return 0;
}

void Server::recording_ready(IPC::Sender const& sender)
{
    if (m_recording.client && m_recording.client == client_of(sender)) {
        m_recording.ready = true;
        if (m_recording.new_since)
            record_frame();
    }
}

void Server::end_recording()
{
    if (m_recording.memory)
        munmap(m_recording.memory, m_recording.memory_size);
    m_recording = {};
}

void Server::stop_recording(IPC::Sender const& sender)
{
    if (m_recording.client && m_recording.client == client_of(sender))
        end_recording();
}

void Server::record_frame()
{
    if (!m_recording.client)
        return;
    m_recording.new_since = true;
    if (!m_recording.ready)
        return;
    int const scale = m_screen.scale();
    auto const origin = m_screen.origin();
    auto const& picture = m_screen.framebuffer();
    IntRect const wanted { (m_recording.points.x + origin.x) * scale, (m_recording.points.y + origin.y) * scale, m_recording.points.width * scale, m_recording.points.height * scale };
    auto const area = wanted.intersected(picture.rect());
    auto* out = static_cast<Pixel*>(m_recording.memory);
    for (int y = 0; y < area.height; ++y)
        __builtin_memcpy(out + static_cast<size_t>(y) * static_cast<size_t>(wanted.width), picture.scanline(area.y + y) + area.x, static_cast<size_t>(area.width) * sizeof(Pixel));
    m_recording.ready = false;
    m_recording.new_since = false;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::RecordedFrame));
    encoder.encode(static_cast<i32>(wanted.width));
    encoder.encode(static_cast<i32>(wanted.height));
    encoder.encode(static_cast<u64>(now_microseconds()));
    send_to(*m_recording.client, encoder);
}

WindowServerEndpoint::RequestAccessibilityAnswer Server::request_accessibility(IPC::Sender const& sender)
{
    WindowServerEndpoint::RequestAccessibilityAnswer answer {};
    if (sender.uid != 0) {
        answer.error = EPERM;
        return answer;
    }
    answer.generation = ++m_accessibility_generation;
    for (auto& client : m_clients) {
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::AccessibilityRequested));
        encoder.encode(answer.generation);
        send_to(*client, encoder);
    }
    return answer;
}

void Server::publish_accessibility(IPC::Sender const& sender, u32 generation, String json)
{
    for (auto& client : m_clients) {
        if (client->pid == sender.pid) {
            client->accessibility_generation = generation;
            client->accessibility = move(json);
            return;
        }
    }
}

WindowServerEndpoint::AccessibilityAnswer Server::accessibility(IPC::Sender const& sender, u32 generation)
{
    WindowServerEndpoint::AccessibilityAnswer answer {};
    if (sender.uid != 0) {
        answer.error = EPERM;
        return answer;
    }
    // Only for clients with a window showing: nothing else could have anything to say.
    auto windows = JSON::Value::array();
    auto answers = JSON::Value::array();
    for (auto const& client : m_clients) {
        bool shows_something = false;
        for (auto const* window : m_windows)
            shows_something = shows_something || (window->client == client.ptr() && window->visible);
        if (!shows_something)
            continue;
        ++answer.asked;
        if (client->accessibility_generation != generation)
            continue;
        ++answer.answered;
        // As it came: this server doesn't need to know what's in it.
        MUST(answers.append(MUST(JSON::Value::string(client->accessibility.view()))));
    }
    // Overview, over everything: told as the server's own, one "window" for each display (ids no window has).
    if (m_overview && !m_overview_leaving) {
        auto own = JSON::Value::array();
        auto const& displays = m_screen.displays();
        for (size_t i = 0; i < displays.size(); ++i) {
            auto const& display = displays[i];
            i64 const id = -1 - static_cast<i64>(i);
            auto one = JSON::Value::object();
            MUST(one.set("id"_sv, JSON::Value(id)));
            MUST(one.set("pid"_sv, JSON::Value(static_cast<i64>(0))));
            MUST(one.set("application"_sv, MUST(JSON::Value::string("WindowServer"_sv))));
            MUST(one.set("title"_sv, MUST(JSON::Value::string("Overview"_sv))));
            MUST(one.set("x"_sv, JSON::Value(display.points.x)));
            MUST(one.set("y"_sv, JSON::Value(display.points.y)));
            MUST(one.set("width"_sv, JSON::Value(display.points.width)));
            MUST(one.set("height"_sv, JSON::Value(display.points.height)));
            MUST(one.set("active"_sv, JSON::Value(false)));
            MUST(windows.append(move(one)));
            auto tree = JSON::Value::object();
            MUST(tree.set("window"_sv, JSON::Value(id)));
            MUST(tree.set("tree"_sv, overview_accessibility(display)));
            MUST(own.append(move(tree)));
        }
        MUST(answers.append(MUST(JSON::Value::string(MUST(own.serialize()).view()))));
    }
    // Stage's strip of other applications, along the left: the server's own too.
    if (m_stage && !stage_strip_is_covered() && !m_stage_strip.is_empty()) {
        auto const& display = m_screen.main_display();
        i64 const id = -100;
        auto one = JSON::Value::object();
        MUST(one.set("id"_sv, JSON::Value(id)));
        MUST(one.set("pid"_sv, JSON::Value(static_cast<i64>(0))));
        MUST(one.set("application"_sv, MUST(JSON::Value::string("WindowServer"_sv))));
        MUST(one.set("title"_sv, MUST(JSON::Value::string("Stage"_sv))));
        MUST(one.set("x"_sv, JSON::Value(display.points.x)));
        MUST(one.set("y"_sv, JSON::Value(display.points.y)));
        MUST(one.set("width"_sv, JSON::Value(display.points.width)));
        MUST(one.set("height"_sv, JSON::Value(display.points.height)));
        MUST(one.set("active"_sv, JSON::Value(false)));
        MUST(windows.append(move(one)));
        auto children = JSON::Value::array();
        for (auto const& item : m_stage_strip) {
            auto button = JSON::Value::object();
            MUST(button.set("role"_sv, MUST(JSON::Value::string("button"_sv))));
            MUST(button.set("label"_sv, MUST(JSON::Value::string(item.client ? item.client->application_name.view() : StringView {}))));
            MUST(button.set("value"_sv, MUST(JSON::Value::string(StringView {}))));
            MUST(button.set("x"_sv, JSON::Value(static_cast<double>(item.rect.x - static_cast<float>(display.points.x)))));
            MUST(button.set("y"_sv, JSON::Value(static_cast<double>(item.rect.y - static_cast<float>(display.points.y)))));
            MUST(button.set("width"_sv, JSON::Value(static_cast<double>(item.rect.width))));
            MUST(button.set("height"_sv, JSON::Value(static_cast<double>(item.rect.height))));
            MUST(button.set("enabled"_sv, JSON::Value(true)));
            MUST(button.set("focused"_sv, JSON::Value(false)));
            MUST(button.set("children"_sv, JSON::Value::array()));
            MUST(children.append(move(button)));
        }
        auto group = JSON::Value::object();
        MUST(group.set("role"_sv, MUST(JSON::Value::string("group"_sv))));
        MUST(group.set("label"_sv, MUST(JSON::Value::string("Stage"_sv))));
        MUST(group.set("value"_sv, MUST(JSON::Value::string(StringView {}))));
        MUST(group.set("x"_sv, JSON::Value(0.0)));
        MUST(group.set("y"_sv, JSON::Value(0.0)));
        MUST(group.set("width"_sv, JSON::Value(static_cast<double>(display.points.width))));
        MUST(group.set("height"_sv, JSON::Value(static_cast<double>(display.points.height))));
        MUST(group.set("enabled"_sv, JSON::Value(true)));
        MUST(group.set("focused"_sv, JSON::Value(false)));
        MUST(group.set("children"_sv, move(children)));
        auto own = JSON::Value::array();
        auto tree = JSON::Value::object();
        MUST(tree.set("window"_sv, JSON::Value(id)));
        MUST(tree.set("tree"_sv, move(group)));
        MUST(own.append(move(tree)));
        MUST(answers.append(MUST(JSON::Value::string(MUST(own.serialize()).view()))));
    }
    // m_windows is back to front.
    for (size_t i = m_windows.size(); i > 0; --i) {
        auto const* window = m_windows[i - 1];
        if (!window->visible)
            continue;
        auto one = JSON::Value::object();
        MUST(one.set("id"_sv, JSON::Value(static_cast<i64>(window->id))));
        MUST(one.set("pid"_sv, JSON::Value(static_cast<i64>(window->client ? window->client->pid : 0))));
        MUST(one.set("application"_sv, MUST(JSON::Value::string(window->client ? window->client->application_name.view() : StringView {}))));
        MUST(one.set("title"_sv, MUST(JSON::Value::string(window->title.view()))));
        MUST(one.set("x"_sv, JSON::Value(window->frame.x)));
        MUST(one.set("y"_sv, JSON::Value(window->frame.y)));
        MUST(one.set("width"_sv, JSON::Value(window->frame.width)));
        MUST(one.set("height"_sv, JSON::Value(window->frame.height)));
        MUST(one.set("active"_sv, JSON::Value(window->active)));
        MUST(windows.append(move(one)));
    }
    auto state = JSON::Value::object();
    MUST(state.set("width"_sv, JSON::Value(m_screen.main_display().points.width)));
    MUST(state.set("height"_sv, JSON::Value(m_screen.main_display().points.height)));
    MUST(state.set("windows"_sv, move(windows)));
    MUST(state.set("answers"_sv, move(answers)));
    auto json = state.serialize();
    if (json.is_error()) {
        answer.error = ENOMEM;
        return answer;
    }
    answer.json = json.release_value();
    return answer;
}

// --- The window manager ---

void Server::activate(ServerWindow* window)
{
    if (m_active == window) {
        if (window)
            raise(*window);
        return;
    }
    if (m_active) {
        // Its shadow shrinks: all it covered while it was big, before it is small.
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
    }
    m_active = window;
    if (window) {
        window->active = true;
        window->chrome_is_stale = true;
        raise(*window);
        m_compositor.invalidate_window(*window);
        if (window->client && !window->client->application_name.is_empty() && m_active_application != window->client) {
            auto* previous_application = m_active_application;
            m_active_application = window->client;
            application_became_active(previous_application);
            shell_state_changed();
        }
        if (window->client) {
            IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Activated));
            encoder.encode(window->id);
            encoder.encode(true);
            send_to(*window->client, encoder);
        }
    }
    schedule_frame();
}

void Server::raise(ServerWindow& window)
{
    if (!m_windows.is_empty() && m_windows.last() == &window)
        return;
    m_windows.remove_all_matching([&](auto* other) { return other == &window; });
    insert_by_level(window);
    m_compositor.invalidate_window(window);
    schedule_frame();
}

void Server::insert_by_level(ServerWindow& window)
{
    // In front of its own level, behind any higher.
    size_t at = m_windows.size();
    while (at > 0 && m_windows[at - 1]->level > window.level)
        --at;
    m_windows.insert(at, &window);
}

void Server::send_moved(ServerWindow& window)
{
    if (!window.client)
        return;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Moved));
    encoder.encode(window.id);
    encoder.encode(static_cast<i32>(window.frame.x));
    encoder.encode(static_cast<i32>(window.frame.y));
    send_to(*window.client, encoder);
}

void Server::send_resized(ServerWindow& window)
{
    if (!window.client)
        return;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Resized));
    encoder.encode(window.id);
    encoder.encode(static_cast<i32>(window.frame.width));
    encoder.encode(static_cast<i32>(window.frame.height));
    send_to(*window.client, encoder);
}

int Server::title_bar_height(ServerWindow const& window) const
{
    return window.title_bar_override > 0 ? window.title_bar_override : m_theme.title_bar_height;
}

IntRect Server::frame_for_arrangement(ServerWindow const& window, Window::WindowArrangement arrangement) const
{
    using Window::WindowArrangement;
    auto const area = work_area_of(m_screen.display_at(window.frame.center()));
    int const gap = m_tile_margins ? static_cast<int>(m_theme.tile_margin) : 0;
    IntRect const inner { area.x + gap, area.y + gap, area.width - gap * 2, area.height - gap * 2 };
    int const half_width = (inner.width - gap) / 2;
    int const half_height = (inner.height - gap) / 2;
    switch (arrangement) {
    case WindowArrangement::Left:
        return { inner.x, inner.y, half_width, inner.height };
    case WindowArrangement::Right:
        return { inner.right() - half_width, inner.y, half_width, inner.height };
    case WindowArrangement::Top:
        return { inner.x, inner.y, inner.width, half_height };
    case WindowArrangement::Bottom:
        return { inner.x, inner.bottom() - half_height, inner.width, half_height };
    case WindowArrangement::TopLeft:
        return { inner.x, inner.y, half_width, half_height };
    case WindowArrangement::TopRight:
        return { inner.right() - half_width, inner.y, half_width, half_height };
    case WindowArrangement::BottomLeft:
        return { inner.x, inner.bottom() - half_height, half_width, half_height };
    case WindowArrangement::BottomRight:
        return { inner.right() - half_width, inner.bottom() - half_height, half_width, half_height };
    case WindowArrangement::Fill:
        return inner;
    case WindowArrangement::Center:
        return { area.x + (area.width - window.frame.width) / 2, area.y + (area.height - window.frame.height) / 2, window.frame.width, window.frame.height };
    case WindowArrangement::Previous:
    default:
        break;
    }
    return window.tile_restore;
}

Vector<Window::WindowArrangement> Server::places_of(Window::WindowArrangement arrangement)
{
    using Window::WindowArrangement;
    Vector<WindowArrangement> places;
    auto add = [&](WindowArrangement a, WindowArrangement b, Optional<WindowArrangement> c = {}, Optional<WindowArrangement> d = {}) {
        places.append(a);
        places.append(b);
        if (c.has_value())
            places.append(*c);
        if (d.has_value())
            places.append(*d);
    };
    switch (arrangement) {
    case WindowArrangement::LeftAndRight:
        add(WindowArrangement::Left, WindowArrangement::Right);
        break;
    case WindowArrangement::RightAndLeft:
        add(WindowArrangement::Right, WindowArrangement::Left);
        break;
    case WindowArrangement::TopAndBottom:
        add(WindowArrangement::Top, WindowArrangement::Bottom);
        break;
    case WindowArrangement::BottomAndTop:
        add(WindowArrangement::Bottom, WindowArrangement::Top);
        break;
    case WindowArrangement::Quarters:
        add(WindowArrangement::TopLeft, WindowArrangement::TopRight, WindowArrangement::BottomLeft, WindowArrangement::BottomRight);
        break;
    case WindowArrangement::LeftAndQuarters:
        add(WindowArrangement::Left, WindowArrangement::TopRight, WindowArrangement::BottomRight);
        break;
    case WindowArrangement::RightAndQuarters:
        add(WindowArrangement::Right, WindowArrangement::TopLeft, WindowArrangement::BottomLeft);
        break;
    case WindowArrangement::TopAndQuarters:
        add(WindowArrangement::Top, WindowArrangement::BottomLeft, WindowArrangement::BottomRight);
        break;
    case WindowArrangement::BottomAndQuarters:
        add(WindowArrangement::Bottom, WindowArrangement::TopLeft, WindowArrangement::TopRight);
        break;
    default:
        break;
    }
    return places;
}

void Server::arrange(ServerWindow& window, Window::WindowArrangement arrangement)
{
    using Window::WindowArrangement;
    // Several together: this one, and those under it on the same display and Space, as they're stacked.
    if (auto places = places_of(arrangement); !places.is_empty()) {
        Vector<ServerWindow*> chosen;
        chosen.append(&window);
        auto const display = m_screen.display_at(window.frame.center()).points;
        for (size_t i = m_windows.size(); i > 0 && chosen.size() < places.size(); --i) {
            auto* other = m_windows[i - 1];
            if (other == &window || other->level != Window::LevelNormal || !other->visible || other->minimized || other->full_screen)
                continue;
            if (!other->is_resizable() || !other->is_document_window() || other->space != window.space || !display.contains(other->frame.center()))
                continue;
            chosen.append(other);
        }
        // From the bottom up, so this one ends on top.
        for (size_t i = chosen.size(); i > 0; --i)
            arrange(*chosen[i - 1], places[i - 1]);
        return;
    }
    if (!window.is_resizable() && arrangement != WindowArrangement::Center)
        return;
    if (arrangement == WindowArrangement::Previous && window.tile_restore.is_empty())
        return;
    auto const frame = frame_for_arrangement(window, arrangement);
    if (arrangement == WindowArrangement::Previous) {
        window.tile = -1;
        window.tile_restore = {};
    } else if (arrangement != WindowArrangement::Center) {
        if (window.tile < 0)
            window.tile_restore = window.frame;
        window.tile = static_cast<int>(arrangement);
    }
    resize_live(window, frame);
    raise(window);
    schedule_frame();
}

int Server::tile_zone_at(IntPoint point, ServerWindow const& window) const
{
    using Window::WindowArrangement;
    if (!window.is_resizable())
        return -1;
    auto const display = m_screen.display_at(point).points;
    int const from_left = point.x - display.x;
    int const from_right = display.right() - 1 - point.x;
    int const from_top = point.y - display.y;
    int const from_bottom = display.bottom() - 1 - point.y;
    // How near the end of an edge is a corner.
    int constexpr corner = 64;
    int constexpr edge = 2;
    // Held down, the zones are out in the open, rather than only at the very edge.
    if ((m_modifiers & Window::ModifierOption) && !m_tile_with_option)
        return -1;
    if (m_modifiers & Window::ModifierOption) {
        bool const left = from_left < display.width / 4;
        bool const right = from_right < display.width / 4;
        bool const top = from_top < display.height / 4;
        bool const bottom = from_bottom < display.height / 4;
        if (left)
            return static_cast<int>(top ? WindowArrangement::TopLeft : (bottom ? WindowArrangement::BottomLeft : WindowArrangement::Left));
        if (right)
            return static_cast<int>(top ? WindowArrangement::TopRight : (bottom ? WindowArrangement::BottomRight : WindowArrangement::Right));
        if (top)
            return static_cast<int>(WindowArrangement::Top);
        if (bottom)
            return static_cast<int>(WindowArrangement::Bottom);
        return -1;
    }
    // Going over to another display isn't going to an edge.
    auto has_neighbor = [&](int dx, int dy) { return m_screen.nearest_point_on_a_display({ point.x + dx, point.y + dy }) == IntPoint { point.x + dx, point.y + dy }; };
    if (m_tile_by_edges && from_left <= edge && !has_neighbor(-edge - 2, 0))
        return static_cast<int>(from_top < corner ? WindowArrangement::TopLeft : (from_bottom < corner ? WindowArrangement::BottomLeft : WindowArrangement::Left));
    if (m_tile_by_edges && from_right <= edge && !has_neighbor(edge + 2, 0))
        return static_cast<int>(from_top < corner ? WindowArrangement::TopRight : (from_bottom < corner ? WindowArrangement::BottomRight : WindowArrangement::Right));
    if (from_top <= edge && !has_neighbor(0, -edge - 2)) {
        // The top's corners are the edges' to tile; between them, it's the menu bar, which fills.
        if (from_left < corner || from_right < corner)
            return m_tile_by_edges ? static_cast<int>(from_left < corner ? WindowArrangement::TopLeft : WindowArrangement::TopRight) : -1;
        return m_tile_to_menu_bar ? static_cast<int>(WindowArrangement::Fill) : -1;
    }
    return -1;
}

void Server::show_tile_preview(int zone, ServerWindow const& window)
{
    if (zone == m_tile_zone)
        return;
    m_tile_zone = zone;
    if (zone < 0) {
        m_compositor.set_tile_preview({});
        return;
    }
    // Where it would be on the display under the pointer, which it hasn't got to yet.
    auto const moved_there = window.frame;
    ServerWindow probe;
    probe.frame = { m_pointer.x, m_pointer.y, moved_there.width, moved_there.height };
    probe.frame.x -= moved_there.width / 2;
    probe.frame.y -= moved_there.height / 2;
    auto const frame = frame_for_arrangement(probe, static_cast<Window::WindowArrangement>(zone));
    m_compositor.set_tile_preview(frame.to<float>());
}

void Server::find_tile_neighbors(ServerWindow& window, int edges)
{
    m_tile_neighbors.clear();
    if (window.tile < 0)
        return;
    int const near = static_cast<int>(m_theme.tile_margin) * 2 + 2;
    auto const& me = window.frame;
    for (auto* other : m_windows) {
        if (other == &window || !other->visible || other->tile < 0)
            continue;
        auto const& them = other->frame;
        bool const overlap_down = them.y < me.bottom() && me.y < them.bottom();
        bool const overlap_across = them.x < me.right() && me.x < them.right();
        int follows = 0;
        if ((edges & edge_right) && overlap_down && abs(them.x - me.right()) <= near)
            follows |= edge_left;
        if ((edges & edge_left) && overlap_down && abs(me.x - them.right()) <= near)
            follows |= edge_right;
        if ((edges & edge_bottom) && overlap_across && abs(them.y - me.bottom()) <= near)
            follows |= edge_top;
        if ((edges & edge_top) && overlap_across && abs(me.y - them.bottom()) <= near)
            follows |= edge_bottom;
        if (follows)
            m_tile_neighbors.append(TileNeighbor { other, them, follows });
    }
}

void Server::arm_window_menu(ServerWindow& window)
{
    if (m_window_menu_window == &window)
        return;
    disarm_window_menu();
    m_window_menu_window = &window;
    m_window_menu_timer = m_loop.add_timer(800, false, [this] {
        m_window_menu_timer = -1;
        auto* window = m_window_menu_window;
        if (!window)
            return;
        for (auto& client : m_clients) {
            if (!client->shows_window_menus)
                continue;
            IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::WindowMenuWanted));
            encoder.encode(window->id);
            encoder.encode(static_cast<i32>(m_pointer.x));
            encoder.encode(static_cast<i32>(m_pointer.y + 8));
            send_to(*client, encoder);
            break;
        }
    });
}

void Server::disarm_window_menu()
{
    if (m_window_menu_timer >= 0)
        m_loop.cancel_timer(m_window_menu_timer);
    m_window_menu_timer = -1;
    m_window_menu_window = nullptr;
}

void Server::arrange_window(IPC::Sender const& sender, u32 id, i32 arrangement)
{
    if (arrangement < 0 || arrangement >= static_cast<i32>(Window::WindowArrangement::Count))
        return;
    auto* window = window_of(sender, id);
    if (!window) {
        // The desktop can do it for anyone: it's what shows the green button's menu.
        auto* client = client_of(sender);
        if (!client || !client->shows_window_menus)
            return;
        for (auto* other : m_windows) {
            if (other->id == id)
                window = other;
        }
    }
    if (window)
        arrange(*window, static_cast<Window::WindowArrangement>(arrangement));
}

void Server::set_tiling(IPC::Sender const&, bool by_edges, bool to_menu_bar, bool with_option, bool margins)
{
    m_tile_by_edges = by_edges;
    m_tile_to_menu_bar = to_menu_bar;
    m_tile_with_option = with_option;
    m_tile_margins = margins;
}

void Server::take_place_of(IPC::Sender const& sender, u32 id, u32 other_id)
{
    auto* window = window_of(sender, id);
    auto* other = window_of(sender, other_id);
    if (!window || !other || window == other)
        return;
    window->space = other->space;
    window->tile = other->tile;
    window->tile_restore = other->tile_restore;
    window->zoom_restore = other->zoom_restore;
    if (other->full_screen) {
        window->full_screen = true;
        window->full_screen_restore = other->full_screen_restore;
        window->full_screen_from = other->full_screen_from;
        other->full_screen = false;
        other->space = other->full_screen_from;
        if (auto* space = space_with_id(window->space)) {
            if (space->full == other)
                space->full = window;
            if (space->beside == other)
                space->beside = window;
        }
    }
    if (window->frame != other->frame) {
        move_window_to(*window, other->frame);
        send_moved(*window);
        send_resized(*window);
    }
    update_visibility(*window);
}

void Server::show_window_menus(IPC::Sender const& sender)
{
    if (auto* client = client_of(sender))
        client->shows_window_menus = true;
}

void Server::hide_others(IPC::Sender const& sender, bool hidden)
{
    auto* me = client_of(sender);
    for (auto& client : m_clients) {
        if (client.ptr() == me || client->application_name.is_empty())
            continue;
        client->hidden = hidden;
    }
    update_all_visibility();
    shell_state_changed();
}

void Server::resize_live(ServerWindow& window, IntRect frame)
{
    if (window.resize_waiting) {
        window.resize_again = frame != window.resize_to;
        window.resize_next = frame;
        return;
    }
    if (frame == window.frame)
        return;
    bool const same_size = frame.size() == window.frame.size();
    // Nobody to wait for; or nothing new for it to draw, or it's taking no notice.
    if (same_size || !window.client || !window.has_presented || window.client->not_responding || window.resize_gave_up) {
        bool const moved = frame.origin() != window.frame.origin();
        move_window_to(window, frame);
        if (moved)
            send_moved(window);
        if (!same_size)
            send_resized(window);
        return;
    }
    window.resize_waiting = true;
    window.resize_to = frame;
    window.resize_asked_at = now_microseconds();
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Resized));
    encoder.encode(window.id);
    encoder.encode(static_cast<i32>(frame.width));
    encoder.encode(static_cast<i32>(frame.height));
    send_to(*window.client, encoder);
    if (m_resize_timer < 0)
        m_resize_timer = m_loop.add_timer(16, true, [this] { check_live_resizes(); });
}

void Server::finish_live_resize(ServerWindow& window)
{
    window.resize_gave_up = false;
    if (!window.resize_waiting)
        return;
    auto const* surface = window.shown_surface();
    int const scale = m_screen.scale();
    // Something of the size it was asked for: there may be others on their way from before.
    if (!surface || surface->width() != window.resize_to.width * scale || surface->height() != window.resize_to.height * scale)
        return;
    window.resize_waiting = false;
    bool const moved = window.resize_to.origin() != window.frame.origin();
    move_window_to(window, window.resize_to);
    if (moved)
        send_moved(window);
    if (window.resize_again) {
        window.resize_again = false;
        resize_live(window, window.resize_next);
    }
}

void Server::check_live_resizes()
{
    u64 const now = now_microseconds();
    bool waiting = false;
    for (auto* window : m_windows) {
        if (!window->resize_waiting)
            continue;
        // A slow program gets a few frames. Past that, the window follows the hand, with what it had.
        if (now - window->resize_asked_at < 100'000) {
            waiting = true;
            continue;
        }
        window->resize_waiting = false;
        window->resize_gave_up = true;
        bool const moved = window->resize_to.origin() != window->frame.origin();
        move_window_to(*window, window->resize_to);
        if (moved)
            send_moved(*window);
        if (window->resize_again) {
            window->resize_again = false;
            resize_live(*window, window->resize_next);
        }
    }
    if (!waiting && m_resize_timer >= 0) {
        m_loop.cancel_timer(m_resize_timer);
        m_resize_timer = -1;
    }
}

void Server::move_window_to(ServerWindow& window, IntRect frame)
{
    if (window.frame == frame)
        return;
    if (m_latency_started == 0)
        m_latency_started = m_pointer_moved_at;
    if (window.visible)
        m_compositor.invalidate_window(window);
    bool resized = window.frame.size() != frame.size();
    window.frame = frame;
    if (resized)
        window.chrome_is_stale = true;
    if (window.visible)
        m_compositor.invalidate_window(window);
    schedule_frame();
}

ServerWindow* Server::window_at(IntPoint point)
{
    auto takes_pointer = [&](ServerWindow const& window) {
        if (window.input_region.is_empty())
            return true;
        return window.input_region.translated(window.frame.x, window.frame.y).contains(point);
    };
    for (size_t i = m_windows.size(); i > 0; --i) {
        auto* window = m_windows[i - 1];
        if (!window->visible)
            continue;
        if (m_drag && m_drag->image == window)
            continue;
        // With the desktop uncovered, windows only look like they're there.
        if (m_desktop_revealed && window->level == Window::LevelNormal)
            continue;
        auto bounds = window->is_resizable() && !window->full_screen ? window->frame.inflated(resize_grab) : window->frame;
        if (bounds.contains(point) && takes_pointer(*window))
            return window;
    }
    return nullptr;
}

WindowButton Server::button_at(ServerWindow const& window, IntPoint point) const
{
    if (!window.is_titled())
        return WindowButton::None;
    float x = static_cast<float>(point.x - window.frame.x);
    float y = static_cast<float>(point.y - window.frame.y);
    float diameter = m_theme.traffic_light_diameter;
    float center_y = static_cast<float>(title_bar_height(window)) / 2;
    WindowButton const buttons[3] = { WindowButton::Close, WindowButton::Minimize, WindowButton::Zoom };
    for (int i = 0; i < 3; ++i) {
        float center_x = m_theme.traffic_light_inset + diameter / 2 + static_cast<float>(i) * (diameter + m_theme.traffic_light_spacing);
        float dx = x - center_x;
        float dy = y - center_y;
        if (dx * dx + dy * dy <= diameter * diameter / 4)
            return buttons[i];
    }
    return WindowButton::None;
}

int Server::resize_edges_at(ServerWindow const& window, IntPoint point) const
{
    if (!window.is_resizable() || window.full_screen)
        return 0;
    auto const& frame = window.frame;
    int edges = 0;
    if (point.x < frame.x + resize_grab)
        edges |= edge_left;
    if (point.x >= frame.right() - resize_grab)
        edges |= edge_right;
    if (point.y < frame.y + resize_grab)
        edges |= edge_top;
    if (point.y >= frame.bottom() - resize_grab)
        edges |= edge_bottom;
    return edges;
}

void Server::invalidate_title_bar(ServerWindow& window)
{
    window.chrome_is_stale = true;
    if (!window.visible || !window.is_titled())
        return;
    auto frame = m_compositor.device_frame(window);
    m_compositor.invalidate({ frame.x, frame.y, frame.width, title_bar_height(window) * m_screen.scale() });
    schedule_frame();
}

void Server::render_chrome(ServerWindow& window)
{
    window.chrome_is_stale = false;
    int scale = m_screen.scale();
    IntSize size { window.frame.width * scale, title_bar_height(window) * scale };
    if (!window.chrome || window.chrome->size() != size) {
        auto bitmap = Bitmap::create(size);
        if (bitmap.is_error())
            return;
        window.chrome = bitmap.release_value();
    }
    auto& bitmap = *window.chrome;
    bool unified = window.is_unified();
    bitmap.clear(unified ? Color { 0, 0, 0, 0 } : (window.active ? m_theme.title_bar_background : m_theme.title_bar_background_inactive));
    Painter painter(bitmap);
    auto scale_f = static_cast<float>(scale);
    if (!unified) {
        // A point of light along the top, and the separator under it: a point too.
        painter.fill_rect({ 0, 0, static_cast<float>(size.width), scale_f }, window.active ? m_theme.title_bar_highlight : m_theme.title_bar_highlight_inactive);
        painter.fill_rect({ 0, static_cast<float>(size.height) - scale_f, static_cast<float>(size.width), scale_f }, m_theme.title_bar_separator);
    }

    // The traffic lights: colored when active, or when the pointer is over
    // them; their glyphs only then.
    bool lit = window.active || window.lights_hovered;
    struct Light {
        WindowButton button;
        Color fill;
        Color border;
    };
    Light const lights[3] = {
        { WindowButton::Close, m_theme.close, m_theme.close_border },
        { WindowButton::Minimize, m_theme.minimize, m_theme.minimize_border },
        { WindowButton::Zoom, m_theme.zoom, m_theme.zoom_border },
    };
    float diameter = m_theme.traffic_light_diameter * scale_f;
    float center_y = static_cast<float>(size.height) / 2;
    for (int i = 0; i < 3; ++i) {
        float center_x = (m_theme.traffic_light_inset + m_theme.traffic_light_diameter / 2 + static_cast<float>(i) * (m_theme.traffic_light_diameter + m_theme.traffic_light_spacing)) * scale_f;
        Path circle;
        circle.add_circle({ center_x, center_y }, diameter / 2);
        painter.fill_path(circle, lit ? lights[i].border : m_theme.traffic_light_inactive_border);
        Path inner;
        inner.add_circle({ center_x, center_y }, diameter / 2 - 0.5f * scale_f);
        painter.fill_path(inner, lit ? lights[i].fill : m_theme.traffic_light_inactive);
        if (!window.lights_hovered)
            continue;
        // The glyph.
        float reach = 3 * scale_f;
        StrokeStyle stroke;
        stroke.width = 1.1f * scale_f;
        stroke.cap = LineCap::Round;
        Path glyph;
        if (lights[i].button == WindowButton::Close) {
            glyph.move_to({ center_x - reach * 0.75f, center_y - reach * 0.75f });
            glyph.line_to({ center_x + reach * 0.75f, center_y + reach * 0.75f });
            glyph.move_to({ center_x + reach * 0.75f, center_y - reach * 0.75f });
            glyph.line_to({ center_x - reach * 0.75f, center_y + reach * 0.75f });
        } else {
            glyph.move_to({ center_x - reach, center_y });
            glyph.line_to({ center_x + reach, center_y });
            if (lights[i].button == WindowButton::Zoom) {
                glyph.move_to({ center_x, center_y - reach });
                glyph.line_to({ center_x, center_y + reach });
            }
        }
        painter.stroke_path(glyph, stroke, m_theme.traffic_light_glyph);
    }

    // A client's own title bar has its own title.
    window.chrome_is_opaque = false;
    if (unified)
        return;
    // The title, centered (and clear of the lights, however narrow the window).
    auto run = m_title_font->shape(window.title.view());
    float lights_end = (m_theme.traffic_light_inset + 3 * m_theme.traffic_light_diameter + 2 * m_theme.traffic_light_spacing + 8) * scale_f;
    float x = max((static_cast<float>(size.width) - run.width) / 2, lights_end);
    float baseline = (static_cast<float>(size.height) + m_title_font->cap_height()) / 2;
    m_title_font->draw(painter, { x, baseline }, run, window.active ? m_theme.title_text : m_theme.title_text_inactive);

    u32 together = 0xffffffffu;
    int const corner = window.shape ? window.shape->corner_extent : 0;
    for (int y = 0; y < bitmap.height(); ++y) {
        auto const* row = bitmap.scanline(y);
        for (int column = corner; column < bitmap.width() - corner; ++column)
            together &= static_cast<u32>(pixel_alpha(row[column]));
    }
    window.chrome_is_opaque = (together & 0xff) == 0xff;
}

// --- Input ---

void Server::read_input()
{
    vulture_input_event events[64];
    for (;;) {
        ssize_t got = read(m_input_fd, events, sizeof(events));
        if (got <= 0)
            return;
        size_t count = static_cast<size_t>(got) / sizeof(vulture_input_event);
        for (size_t i = 0; i < count; ++i) {
            auto const& event = events[i];
            m_event_time = event.time;
            switch (event.type) {
            case VULTURE_EV_HEAD:
                m_pending_head = event.value;
                break;
            case VULTURE_EV_ABS:
                touch_event_read(event.code, event.value);
                if (event.code == VULTURE_ABS_X)
                    m_pending_absolute_x = event.value;
                else if (event.code == VULTURE_ABS_Y)
                    m_pending_absolute_y = event.value;
                break;
            case VULTURE_EV_REL:
                if (event.code == VULTURE_REL_WHEEL)
                    wheel_moved(0, event.value);
                else if (event.code == VULTURE_REL_HWHEEL)
                    wheel_moved(event.value, 0);
                else if (event.code == VULTURE_REL_X)
                    m_pending_relative_x += event.value;
                else if (event.code == VULTURE_REL_Y)
                    m_pending_relative_y += event.value;
                break;
            case VULTURE_EV_KEY: {
                // What's touching is said by the fingers themselves.
                if (event.code == VULTURE_BTN_TOUCH)
                    break;
                u32 button = 0;
                if (event.code == VULTURE_BTN_LEFT)
                    button = Window::ButtonLeft;
                else if (event.code == VULTURE_BTN_RIGHT)
                    button = Window::ButtonRight;
                else if (event.code == VULTURE_BTN_MIDDLE)
                    button = Window::ButtonMiddle;
                if (button) {
                    u32 buttons = event.value ? (m_buttons | button) : (m_buttons & ~button);
                    pointer_changed(m_pointer, buttons);
                } else {
                    key_changed(event.code, event.value);
                }
                break;
            }
            case VULTURE_EV_SYN: {
                touches_finished();
                if (m_pending_absolute_x >= 0 || m_pending_absolute_y >= 0) {
                    auto const display = m_screen.display_of_head(m_pending_head).points;
                    IntPoint target = m_pointer;
                    if (m_pending_absolute_x >= 0)
                        target.x = display.x + static_cast<int>(static_cast<i64>(m_pending_absolute_x) * display.width / (VULTURE_INPUT_ABSOLUTE_MAXIMUM + 1));
                    if (m_pending_absolute_y >= 0)
                        target.y = display.y + static_cast<int>(static_cast<i64>(m_pending_absolute_y) * display.height / (VULTURE_INPUT_ABSOLUTE_MAXIMUM + 1));
                    m_pending_absolute_x = m_pending_absolute_y = -1;
                    pointer_changed(target, m_buttons);
                }
                // A mouse, which only says how far it went. A point on the screen is the most that can
                // be asked of it: a device pixel, at any scale, is less than one of those anyway.
                if (m_pending_relative_x != 0 || m_pending_relative_y != 0) {
                    // Over to the next display, if there's one there; along the edge if not.
                    IntPoint target = m_screen.nearest_point_on_a_display({ m_pointer.x + m_pending_relative_x * m_pointer_speed / 100, m_pointer.y + m_pending_relative_y * m_pointer_speed / 100 });
                    m_pending_relative_x = m_pending_relative_y = 0;
                    pointer_changed(target, m_buttons);
                }
                break;
            }
            default:
                break;
            }
        }
    }
}

void Server::send_mouse(ServerWindow& window, MouseKind kind, IntPoint point, u32 buttons, int wheel_x, int wheel_y)
{
    if (!window.client)
        return;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Mouse));
    encoder.encode(window.id);
    encoder.encode(static_cast<u8>(kind));
    encoder.encode(static_cast<i32>(point.x - window.frame.x));
    encoder.encode(static_cast<i32>(point.y - window.frame.y));
    encoder.encode(buttons);
    encoder.encode(m_modifiers);
    encoder.encode(static_cast<i32>(wheel_x));
    encoder.encode(static_cast<i32>(wheel_y));
    send_to(*window.client, encoder);
    ask_if_responding(*window.client);
}

void Server::pointer_changed(IntPoint point, u32 buttons)
{
    // When it happened, if that's known, as it may have waited here for a frame to finish.
    u64 const now = now_microseconds();
    m_pointer_moved_at = m_event_time != 0 && m_event_time <= now && now - m_event_time < 1'000'000 ? m_event_time : now;
    note_input();
    point = m_screen.nearest_point_on_a_display(point);
    bool moved = point != m_pointer;
    u32 pressed = buttons & ~m_buttons;
    u32 released = m_buttons & ~buttons;
    m_pointer = point;
    m_buttons = buttons;
    if (moved)
        m_screen.move_cursor({ (point.x + m_screen.origin().x) * m_screen.scale(), (point.y + m_screen.origin().y) * m_screen.scale() });

    if (m_drag) {
        drag_pointer_changed(point, released);
        update_cursor();
        schedule_frame();
        return;
    }
    if (moved && !m_grab) {
        check_full_screen_edges(point);
        check_hot_corner(point);
    }
    if (m_overview && overview_pointer(point, pressed, released)) {
        update_cursor();
        schedule_frame();
        return;
    }
    if (m_grab) {
        auto& grab = *m_grab;
        if (moved)
            send_mouse(grab, MouseKind::Moved, point, buttons);
        if (pressed)
            send_mouse(grab, MouseKind::Pressed, point, buttons);
        if (released)
            send_mouse(grab, MouseKind::Released, point, buttons);
        update_cursor();
        schedule_frame();
        return;
    }

    switch (m_gesture) {
    case Gesture::Moving:
        if (m_gesture_window && moved) {
            // A tiled window, taken away, goes back to the size it was, with the same part of it under the pointer.
            if (m_gesture_window->tile >= 0 && !m_gesture_window->tile_restore.is_empty() && (abs(point.x - m_gesture_start.x) > 4 || abs(point.y - m_gesture_start.y) > 4)) {
                auto const restore = m_gesture_window->tile_restore;
                float const along = static_cast<float>(m_gesture_start.x - m_gesture_frame.x) / static_cast<float>(max(m_gesture_frame.width, 1));
                m_gesture_frame = { point.x - static_cast<int>(along * static_cast<float>(restore.width)), point.y - (m_gesture_start.y - m_gesture_frame.y), restore.width, restore.height };
                m_gesture_start = point;
                m_gesture_window->tile = -1;
                m_gesture_window->tile_restore = {};
                resize_live(*m_gesture_window, m_gesture_frame);
            }
            auto frame = m_gesture_frame.translated(point.x - m_gesture_start.x, point.y - m_gesture_start.y);
            // The title bar stays reachable.
            frame.y = max(frame.y, m_screen.display_at(point).points.y);
            if (frame.size() == m_gesture_window->frame.size()) {
                move_window_to(*m_gesture_window, frame);
                send_moved(*m_gesture_window);
            }
            show_tile_preview(tile_zone_at(point, *m_gesture_window), *m_gesture_window);
        }
        break;
    case Gesture::Resizing:
        if (m_gesture_window && moved) {
            int dx = point.x - m_gesture_start.x;
            int dy = point.y - m_gesture_start.y;
            auto frame = m_gesture_frame;
            if (m_gesture_edges & edge_left) {
                int width = max(frame.width - dx, minimum_width);
                frame.x = frame.right() - width;
                frame.width = width;
            } else if (m_gesture_edges & edge_right) {
                frame.width = max(frame.width + dx, minimum_width);
            }
            if (m_gesture_edges & edge_top) {
                int height = max(frame.height - dy, minimum_height);
                frame.y = frame.bottom() - height;
                frame.height = height;
            } else if (m_gesture_edges & edge_bottom) {
                frame.height = max(frame.height + dy, minimum_height);
            }
            resize_live(*m_gesture_window, frame);
            // Those tiled next to it give way, or fill in.
            int const moved_right = frame.right() - m_gesture_frame.right();
            int const moved_left = frame.x - m_gesture_frame.x;
            int const moved_bottom = frame.bottom() - m_gesture_frame.bottom();
            int const moved_top = frame.y - m_gesture_frame.y;
            for (auto const& neighbor : m_tile_neighbors) {
                auto next = neighbor.start;
                if (neighbor.edges & edge_left) {
                    next.x += moved_right;
                    next.width -= moved_right;
                }
                if (neighbor.edges & edge_right)
                    next.width += moved_left;
                if (neighbor.edges & edge_top) {
                    next.y += moved_bottom;
                    next.height -= moved_bottom;
                }
                if (neighbor.edges & edge_bottom)
                    next.height += moved_top;
                if (next.width >= minimum_width && next.height >= minimum_height)
                    resize_live(*neighbor.window, next);
            }
        }
        break;
    case Gesture::ContentPress:
        if (m_gesture_window && moved)
            send_mouse(*m_gesture_window, MouseKind::Moved, point, buttons);
        break;
    default:
        break;
    }

    if (released) {
        if (m_gesture == Gesture::Moving && m_tile_zone >= 0 && m_gesture_window) {
            arrange(*m_gesture_window, static_cast<Window::WindowArrangement>(m_tile_zone));
            // Its previous size is where it was before this, not where it was let go.
            if (!m_gesture_restore.is_empty())
                m_gesture_window->tile_restore = m_gesture_restore;
        }
        if (m_tile_zone >= 0) {
            m_tile_zone = -1;
            m_compositor.set_tile_preview({});
        }
        if (m_gesture == Gesture::Moving && m_gesture_window)
            settle_on_display(*m_gesture_window);
        m_dragging_divider = false;
        m_tile_neighbors.clear();
        if (m_gesture_window) {
            if (m_gesture == Gesture::ButtonPress) {
                auto button = button_at(*m_gesture_window, point);
                auto pressed_button = m_gesture_window->pressed_button;
                m_gesture_window->pressed_button = WindowButton::None;
                invalidate_title_bar(*m_gesture_window);
                if (button == pressed_button && button != WindowButton::None) {
                    if (button == WindowButton::Close && m_gesture_window->client) {
                        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::CloseRequested));
                        encoder.encode(m_gesture_window->id);
                        send_to(*m_gesture_window->client, encoder);
                    } else if (button == WindowButton::Minimize) {
                        minimize_window(*m_gesture_window);
                    } else if (button == WindowButton::Zoom) {
                        // With Option it only gets as big as it can, as it used to.
                        if (m_gesture_window->full_screen)
                            leave_full_screen(*m_gesture_window, false);
                        else if ((m_modifiers & Window::ModifierOption) || !m_gesture_window->is_resizable())
                            zoom_window(*m_gesture_window);
                        else
                            enter_full_screen(*m_gesture_window);
                    }
                }
            } else if (m_gesture == Gesture::ContentPress) {
                send_mouse(*m_gesture_window, MouseKind::Released, point, buttons);
            }
        }
        if (buttons == 0) {
            m_gesture = Gesture::None;
            m_gesture_window = nullptr;
        }
    }

    if (m_dragging_divider && moved)
        drag_divider(point);
    if (pressed && m_gesture == Gesture::None) {
        auto* window = window_at(point);
        // What the server draws over the desktop (the line between two halves of a Space, Stage's strip) comes before
        // the desktop's own window, under it.
        bool const over_desktop = !window || window->level == Window::LevelDesktop;
        if (over_desktop && split_divider_at(point).has_value()) {
            m_dragging_divider = true;
            window = nullptr;
        } else if (over_desktop && stage_strip_pointer(point)) {
            update_cursor();
            schedule_frame();
            return;
        } else if (!window) {
            // The wallpaper. (Where the desktop has a window of its own, it's what says, as only it knows what's an icon.)
            press_on_wallpaper(point, true);
        }
        if (window && window->can_become_key())
            activate(window);
        if (window) {
            m_gesture_window = window;
            m_gesture_start = point;
            m_gesture_frame = window->frame;
            m_gesture_restore = window->tile >= 0 && !window->tile_restore.is_empty() ? window->tile_restore : window->frame;
            auto button = button_at(*window, point);
            int edges = resize_edges_at(*window, point);
            bool in_title_bar = !window->full_screen && window->is_titled() && !window->is_unified() && point.y < window->frame.y + title_bar_height(*window);
            if (button != WindowButton::None) {
                m_gesture = Gesture::ButtonPress;
                window->pressed_button = button;
                invalidate_title_bar(*window);
            } else if (edges) {
                m_gesture = Gesture::Resizing;
                m_gesture_edges = edges;
                find_tile_neighbors(*window, edges);
            } else if (in_title_bar) {
                m_gesture = Gesture::Moving;
            } else {
                m_gesture = Gesture::ContentPress;
                send_mouse(*window, MouseKind::Pressed, point, buttons);
            }
        }
    }

    // What the pointer is over, when nothing has it captured.
    if (m_gesture == Gesture::None) {
        auto* window = window_at(point);
        if (window != m_hovered) {
            if (m_hovered) {
                send_mouse(*m_hovered, MouseKind::Left, point, buttons);
                if (m_hovered->lights_hovered) {
                    m_hovered->lights_hovered = false;
                    invalidate_title_bar(*m_hovered);
                }
            }
            m_hovered = window;
            if (window)
                send_mouse(*window, MouseKind::Entered, point, buttons);
        } else if (window && moved) {
            send_mouse(*window, MouseKind::Moved, point, buttons);
        }
        if (!window || !window->is_titled())
            disarm_window_menu();
        if (window && window->is_titled()) {
            // Any of the three lights lights up the glyphs on all.
            float top = static_cast<float>(window->frame.y);
            float diameter = m_theme.traffic_light_diameter;
            float group_right = static_cast<float>(window->frame.x) + m_theme.traffic_light_inset + 3 * diameter + 2 * m_theme.traffic_light_spacing;
            float group_top = top + (static_cast<float>(title_bar_height(*window)) - diameter) / 2;
            bool over = static_cast<float>(point.x) >= static_cast<float>(window->frame.x) + m_theme.traffic_light_inset
                && static_cast<float>(point.x) < group_right && static_cast<float>(point.y) >= group_top && static_cast<float>(point.y) < group_top + diameter;
            if (over != window->lights_hovered) {
                window->lights_hovered = over;
                invalidate_title_bar(*window);
            }
            if (button_at(*window, point) == WindowButton::Zoom)
                arm_window_menu(*window);
            else
                disarm_window_menu();
        }
    }
    update_cursor();
    schedule_frame();
}

void Server::send_drag(ServerWindow& window, Window::DragPhase phase, IntPoint point, bool with_data)
{
    if (!window.client || !m_drag)
        return;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Drag));
    encoder.encode(window.id);
    encoder.encode(static_cast<u8>(phase));
    encoder.encode(static_cast<i32>(point.x - window.frame.x));
    encoder.encode(static_cast<i32>(point.y - window.frame.y));
    encoder.encode(m_drag->type.view());
    if (with_data)
        encoder.encode(m_drag->data.span());
    else
        encoder.encode(ReadonlyBytes {});
    send_to(*window.client, encoder);
}

void Server::drag_pointer_changed(IntPoint point, u32 released)
{
    auto& drag = *m_drag;
    move_window_to(*drag.image, drag.image->frame.size().is_empty() ? drag.image->frame : IntRect { point.x + drag.image_offset.x, point.y + drag.image_offset.y, drag.image->frame.width, drag.image->frame.height });
    auto* over = window_at(point);
    if (over != drag.target) {
        if (drag.target)
            send_drag(*drag.target, Window::DragPhase::Left, point, false);
        drag.target = over;
        if (over) {
            over->accepts_drop = false;
            send_drag(*over, Window::DragPhase::Entered, point, false);
        }
    } else if (over) {
        send_drag(*over, Window::DragPhase::Moved, point, false);
    }
    if (!released)
        return;
    bool accepted = drag.target && drag.target->accepts_drop;
    if (accepted)
        send_drag(*drag.target, Window::DragPhase::Dropped, point, true);
    else if (drag.target)
        send_drag(*drag.target, Window::DragPhase::Left, point, false);
    auto* source = drag.source;
    m_drag = nullptr;
    if (source && source->client) {
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::DragFinished));
        encoder.encode(source->id);
        encoder.encode(accepted);
        send_to(*source->client, encoder);
    }
}

void Server::wheel_moved(int x, int y)
{
    auto* window = m_grab ? m_grab : window_at(m_pointer);
    if (window) {
        int const direction = m_natural_scrolling ? 1 : -1;
        send_mouse(*window, MouseKind::Wheel, m_pointer, m_buttons, x * direction, y * direction);
    }
}

void Server::touch_event_read(u16 code, int value)
{
    switch (code) {
    case VULTURE_ABS_MT_SLOT:
        m_touch_slot = clamp(value, 0, touch_slots - 1);
        break;
    case VULTURE_ABS_MT_TRACKING_ID:
        m_touches[m_touch_slot].down = value >= 0;
        break;
    case VULTURE_ABS_MT_POSITION_X:
        m_touches[m_touch_slot].x = value;
        m_touches[m_touch_slot].moved = true;
        break;
    case VULTURE_ABS_MT_POSITION_Y:
        m_touches[m_touch_slot].y = value;
        m_touches[m_touch_slot].moved = true;
        break;
    default:
        break;
    }
}

void Server::touches_finished()
{
    auto const display = m_screen.main_display().points;
    double const now = static_cast<double>(now_microseconds()) / 1'000'000.0;
    auto point_of = [&](Touch const& touch) {
        return FloatPoint {
            static_cast<float>(display.x) + static_cast<float>(touch.x) * static_cast<float>(display.width) / static_cast<float>(VULTURE_INPUT_ABSOLUTE_MAXIMUM + 1),
            static_cast<float>(display.y) + static_cast<float>(touch.y) * static_cast<float>(display.height) / static_cast<float>(VULTURE_INPUT_ABSOLUTE_MAXIMUM + 1),
        };
    };
    if (!m_gestures.on_event)
        m_gestures.on_event = [this](::Gesture::Event const& event) { handle_gesture(event); };
    for (int slot = 0; slot < touch_slots; ++slot) {
        auto& touch = m_touches[slot];
        auto const point = point_of(touch);
        if (touch.down && !touch.was_down)
            m_gestures.finger_down(slot, point.x, point.y, now);
        else if (!touch.down && touch.was_down)
            m_gestures.finger_up(slot, now);
        else if (touch.down && touch.moved)
            m_gestures.finger_moved(slot, point.x, point.y, now);
        touch.was_down = touch.down;
        touch.moved = false;
    }
}

void Server::handle_gesture(::Gesture::Event const& event)
{
    using Kind = ::Gesture::Kind;
    IntPoint const at { static_cast<int>(event.x), static_cast<int>(event.y) };
    auto* window = m_grab ? m_grab : window_at(m_pointer);
    switch (event.kind) {
    case Kind::PointerMoved:
        pointer_changed(at, m_buttons);
        break;
    case Kind::Tap:
        if (!m_tap_to_click) {
            pointer_changed(at, m_buttons);
            break;
        }
        pointer_changed(at, m_buttons | Window::ButtonLeft);
        pointer_changed(at, m_buttons & ~Window::ButtonLeft);
        break;
    case Kind::SecondaryTap:
        pointer_changed(at, m_buttons | Window::ButtonRight);
        pointer_changed(at, m_buttons & ~Window::ButtonRight);
        break;
    case Kind::Scrolled:
        // Natural: what's under the fingers goes with them, so going up is further down it.
        if (window) {
            float const direction = m_natural_scrolling ? -1 : 1;
            send_mouse(*window, MouseKind::Scroll, m_pointer, m_buttons, static_cast<int>(event.x * direction), static_cast<int>(event.y * direction));
        }
        break;
    case Kind::Pinched:
        if (window)
            send_mouse(*window, MouseKind::Magnify, m_pointer, m_buttons, static_cast<int>(event.scale * 1000), 0);
        break;
    case Kind::SwipeChanged:
        swipe_changed(event.x, event.y, false);
        break;
    case Kind::SwipeEnded:
        swipe_changed(event.x, event.y, true);
        break;
    case Kind::ThreeFingersUp:
        if (!m_overview)
            enter_overview();
        break;
    case Kind::ThreeFingersDown:
        if (m_overview)
            leave_overview(nullptr);
        break;
    case Kind::ScrollEnded:
        break;
    }
    schedule_frame();
}

void Server::set_natural_scrolling(IPC::Sender const&, bool natural)
{
    m_natural_scrolling = natural;
}

void Server::set_tap_to_click(IPC::Sender const&, bool tap)
{
    m_tap_to_click = tap;
}

void Server::set_pointer_speed(IPC::Sender const&, i32 percent)
{
    m_pointer_speed = clamp(percent, 25, 400);
}

void Server::key_changed(u16 code, int value)
{
    u32 modifier = 0;
    switch (code) {
    case VULTURE_KEY_LEFTSHIFT:
    case VULTURE_KEY_RIGHTSHIFT:
        modifier = Window::ModifierShift;
        break;
    case VULTURE_KEY_LEFTCTRL:
    case VULTURE_KEY_RIGHTCTRL:
        modifier = Window::ModifierControl;
        break;
    case VULTURE_KEY_LEFTALT:
    case VULTURE_KEY_RIGHTALT:
        modifier = Window::ModifierOption;
        break;
    case VULTURE_KEY_LEFTMETA:
    case VULTURE_KEY_RIGHTMETA:
        modifier = Window::ModifierCommand;
        break;
    default:
        break;
    }
    if (modifier)
        m_modifiers = value ? (m_modifiers | modifier) : (m_modifiers & ~modifier);
    // Cmd let go: wherever Cmd-Tab had got to.
    if (modifier == Window::ModifierCommand && !value)
        finish_switching(true);
    if (code == VULTURE_KEY_CAPSLOCK && value == 1)
        m_modifiers ^= Window::ModifierCapsLock;
    // Keys repeat because this says so, as on a Mac, however long the machine that's showing this holds them down for, and whatever it says about it.
    if (value == 2 || (value == 1 && code == m_repeating_key))
        return;
    if (value == 1 && handle_hotkey(code))
        return;
    if (value == 1 && !modifier && !(m_modifiers & Window::ModifierCommand)) {
        start_key_repeat(code);
    } else if (value == 0 && code == m_repeating_key) {
        stop_key_repeat();
    }
    deliver_key(code, value);
}

void Server::start_key_repeat(u16 code)
{
    stop_key_repeat();
    m_repeating_key = code;
    // A Mac's defaults: a wait, and then a steady stream until it's let go.
    constexpr u64 delay_until_repeat = 375;
    constexpr u64 between_repeats = 90;
    m_key_repeat_timer = m_loop.add_timer(delay_until_repeat, false, [this] {
        deliver_key(m_repeating_key, 2);
        m_key_repeat_timer = m_loop.add_timer(between_repeats, true, [this] { deliver_key(m_repeating_key, 2); });
    });
}

void Server::stop_key_repeat()
{
    if (m_key_repeat_timer >= 0)
        m_loop.cancel_timer(m_key_repeat_timer);
    m_key_repeat_timer = -1;
    m_repeating_key = 0;
}

void Server::deliver_key(u16 code, int value)
{
    // Keys go to the active window, unless some other program has a menu open: Escape and the arrows are that menu's.
    auto* target = m_active;
    if (m_grab && (m_grab->flags & Window::WindowPopup) && m_grab->client && (!m_active || m_grab->client != m_active->client))
        target = m_grab;
    if (!target || !target->client)
        return;
    // What it types, by the layout that's chosen. Only when it's pressed, as a key that's let go of types nothing.
    String text;
    if (value) {
        bool const shift = m_modifiers & Window::ModifierShift;
        bool const option = m_modifiers & Window::ModifierOption;
        bool const caps = m_modifiers & Window::ModifierCapsLock;
        bool const shortcut = m_modifiers & (Window::ModifierCommand | Window::ModifierControl);
        text = m_keyboard_layout.type(code, shift, option, caps, shortcut);
    }
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Key));
    encoder.encode(target->id);
    encoder.encode(value != 0);
    encoder.encode(code);
    encoder.encode(value == 2 ? (m_modifiers | Window::ModifierRepeat) : m_modifiers);
    encoder.encode(text.view());
    send_to(*target->client, encoder);
    ask_if_responding(*target->client);
}

}
