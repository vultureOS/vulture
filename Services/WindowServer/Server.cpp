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
    auto points = screen.points();
    m_pointer = { points.width / 2, points.height / 2 };
}

void Server::start()
{
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
    m_screen.move_cursor({ m_pointer.x * m_screen.scale(), m_pointer.y * m_screen.scale() });
}

void Server::update_cursor()
{
    using Window::CursorShape;
    CursorShape wanted = CursorShape::Arrow;
    int edges = 0;
    if (m_drag) {
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
    m_compositor.render();
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
    auto points = m_screen.points();
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
    return answer;
}

void Server::remove_client(Client& client)
{
    os_log_info(server_log(), "A client is gone: %s", client.application_name.is_empty() ? "(not an application)" : client.application_name.characters());
    for (size_t i = m_windows.size(); i > 0; --i) {
        if (m_windows[i - 1]->client == &client)
            remove_window(*m_windows[i - 1]);
    }
    m_loop.unwatch(client.watch);
    m_hotkeys.remove_all_matching([&](auto const& hotkey) { return hotkey.client == &client; });
    if (m_active_application == &client)
        m_active_application = nullptr;
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
    m_compositor.invalidate_window(window);
    release_surface(window);
    if (m_active == &window)
        m_active = nullptr;
    if (m_hovered == &window)
        m_hovered = nullptr;
    if (m_gesture_window == &window) {
        m_gesture_window = nullptr;
        m_gesture = Gesture::None;
    }
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
    window->space = m_space;
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
    if (visible)
        window->space = window->is_on_every_space() ? window->space : m_space;
    update_visibility(*window);
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
    release_surface(*window);
    window->memory = mapped;
    window->memory_size = total;
    auto* first = static_cast<Pixel*>(mapped);
    auto one = Bitmap::wrap(first, { width, height }, width);
    auto two = Bitmap::wrap(first + static_cast<size_t>(width) * static_cast<size_t>(height), { width, height }, width);
    if (one.is_error() || two.is_error())
        return ENOMEM;
    window->buffers[0] = one.release_value();
    window->buffers[1] = two.release_value();
    window->shown_buffer = 0;
    m_compositor.surface_replaced(*window);
    return 0;
}

void Server::present(IPC::Sender const& sender, u32 id, u32 buffer, i32 x, i32 y, i32 width, i32 height)
{
    auto* window = window_of(sender, id);
    if (!window || buffer > 1 || !window->buffers[0])
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
    return { m_compositor.frames(), m_compositor.compose_microseconds(), m_compositor.flush_microseconds(), m_compositor.pixels(), m_compositor.material_microseconds() };
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

WindowServerEndpoint::CaptureAnswer Server::capture(IPC::Sender const& sender)
{
    WindowServerEndpoint::CaptureAnswer answer {};
    if (sender.uid != 0) {
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
    MUST(state.set("width"_sv, JSON::Value(m_screen.points().width)));
    MUST(state.set("height"_sv, JSON::Value(m_screen.points().height)));
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
            m_active_application = window->client;
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
        auto bounds = window->is_resizable() ? window->frame.inflated(resize_grab) : window->frame;
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
    if (!window.is_resizable())
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
            case VULTURE_EV_ABS:
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
                if (m_pending_absolute_x >= 0 || m_pending_absolute_y >= 0) {
                    auto points = m_screen.points();
                    IntPoint target = m_pointer;
                    if (m_pending_absolute_x >= 0)
                        target.x = static_cast<int>(static_cast<i64>(m_pending_absolute_x) * points.width / (VULTURE_INPUT_ABSOLUTE_MAXIMUM + 1));
                    if (m_pending_absolute_y >= 0)
                        target.y = static_cast<int>(static_cast<i64>(m_pending_absolute_y) * points.height / (VULTURE_INPUT_ABSOLUTE_MAXIMUM + 1));
                    m_pending_absolute_x = m_pending_absolute_y = -1;
                    pointer_changed(target, m_buttons);
                }
                // A mouse, which only says how far it went. A point on the screen is the most that can
                // be asked of it: a device pixel, at any scale, is less than one of those anyway.
                if (m_pending_relative_x != 0 || m_pending_relative_y != 0) {
                    auto points = m_screen.points();
                    IntPoint target { clamp(m_pointer.x + m_pending_relative_x, 0, points.width - 1), clamp(m_pointer.y + m_pending_relative_y, 0, points.height - 1) };
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
    auto points = m_screen.points();
    point.x = clamp(point.x, 0, points.width - 1);
    point.y = clamp(point.y, 0, points.height - 1);
    bool moved = point != m_pointer;
    u32 pressed = buttons & ~m_buttons;
    u32 released = m_buttons & ~buttons;
    m_pointer = point;
    m_buttons = buttons;
    if (moved)
        m_screen.move_cursor({ point.x * m_screen.scale(), point.y * m_screen.scale() });

    if (m_drag) {
        drag_pointer_changed(point, released);
        update_cursor();
        schedule_frame();
        return;
    }
    if (m_overview && overview_pointer(point, pressed)) {
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
            auto frame = m_gesture_frame.translated(point.x - m_gesture_start.x, point.y - m_gesture_start.y);
            // The title bar stays reachable.
            frame.y = max(frame.y, 0);
            move_window_to(*m_gesture_window, frame);
            send_moved(*m_gesture_window);
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
            bool origin_changed = frame.x != m_gesture_window->frame.x || frame.y != m_gesture_window->frame.y;
            move_window_to(*m_gesture_window, frame);
            if (origin_changed)
                send_moved(*m_gesture_window);
            send_resized(*m_gesture_window);
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
                        zoom_window(*m_gesture_window);
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

    if (pressed && m_gesture == Gesture::None) {
        auto* window = window_at(point);
        if (window && window->can_become_key())
            activate(window);
        else if (!window)
            activate(nullptr);
        if (window) {
            m_gesture_window = window;
            m_gesture_start = point;
            m_gesture_frame = window->frame;
            auto button = button_at(*window, point);
            int edges = resize_edges_at(*window, point);
            bool in_title_bar = window->is_titled() && !window->is_unified() && point.y < window->frame.y + title_bar_height(*window);
            if (button != WindowButton::None) {
                m_gesture = Gesture::ButtonPress;
                window->pressed_button = button;
                invalidate_title_bar(*window);
            } else if (edges) {
                m_gesture = Gesture::Resizing;
                m_gesture_edges = edges;
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
    if (window)
        send_mouse(*window, MouseKind::Wheel, m_pointer, m_buttons, x, y);
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
