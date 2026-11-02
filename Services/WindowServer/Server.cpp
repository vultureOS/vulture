/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <os/log.h>
#include "Server.h"
#include "Cursors.h"
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

// Key code to text, on a US keyboard.
struct KeyPair {
    char plain;
    char shifted;
};

constexpr KeyPair us_layout[] = {
    { 0, 0 }, { 0, 0 }, { '1', '!' }, { '2', '@' }, { '3', '#' }, { '4', '$' }, { '5', '%' }, { '6', '^' },
    { '7', '&' }, { '8', '*' }, { '9', '(' }, { '0', ')' }, { '-', '_' }, { '=', '+' }, { 8, 8 }, { '\t', '\t' },
    { 'q', 'Q' }, { 'w', 'W' }, { 'e', 'E' }, { 'r', 'R' }, { 't', 'T' }, { 'y', 'Y' }, { 'u', 'U' }, { 'i', 'I' },
    { 'o', 'O' }, { 'p', 'P' }, { '[', '{' }, { ']', '}' }, { '\r', '\r' }, { 0, 0 }, { 'a', 'A' }, { 's', 'S' },
    { 'd', 'D' }, { 'f', 'F' }, { 'g', 'G' }, { 'h', 'H' }, { 'j', 'J' }, { 'k', 'K' }, { 'l', 'L' }, { ';', ':' },
    { '\'', '"' }, { '`', '~' }, { 0, 0 }, { '\\', '|' }, { 'z', 'Z' }, { 'x', 'X' }, { 'c', 'C' }, { 'v', 'V' },
    { 'b', 'B' }, { 'n', 'N' }, { 'm', 'M' }, { ',', '<' }, { '.', '>' }, { '/', '?' }, { 0, 0 }, { '*', '*' },
    { 0, 0 }, { ' ', ' ' },
};

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
    auto cursor = make_cursor(m_cursor_shape, m_screen.scale());
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
}

// --- Frames ---

void Server::schedule_frame()
{
    if (m_frame_scheduled || !m_compositor.has_damage())
        return;
    m_frame_scheduled = true;
    // Frames keep to a fixed beat, as a display's refresh would: a timer that
    // fires late (they do, waking an idle processor under a hypervisor) takes
    // that time from the wait for the next, rather than slowing them all down.
    u64 now = now_microseconds();
    if (m_next_frame_time + frame_interval < now)
        m_next_frame_time = now;
    u64 delay = m_next_frame_time > now ? (m_next_frame_time - now) / 1000 : 0;
    m_loop.add_timer(delay, false, [this] { render_frame(); });
}

void Server::render_frame()
{
    m_frame_scheduled = false;
    m_next_frame_time += frame_interval;
    // Title bars out of date are redrawn now, once, however many changes came.
    for (auto* window : m_windows) {
        if (window->visible && window->is_titled() && window->chrome_is_stale)
            render_chrome(*window);
    }
    m_compositor.render();
    // What the clients presented is on screen: they may draw on.
    for (auto* window : m_windows) {
        if (!window->present_pending || !window->client)
            continue;
        window->present_pending = false;
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Presented));
        encoder.encode(window->id);
        send_to(*window->client, encoder);
    }
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

void Server::inject_pointer(IPC::Sender const& sender, i32 x, i32 y, u32 buttons)
{
    if (sender.uid != 0)
        return;
    pointer_changed({ x, y }, buttons);
}

WindowServerEndpoint::StatisticsAnswer Server::statistics(IPC::Sender const&)
{
    return { m_compositor.frames(), m_compositor.compose_microseconds(), m_compositor.flush_microseconds(), m_compositor.pixels() };
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
    if (unified)
        return;
    // The title, centered (and clear of the lights, however narrow the window).
    auto run = m_title_font->shape(window.title.view());
    float lights_end = (m_theme.traffic_light_inset + 3 * m_theme.traffic_light_diameter + 2 * m_theme.traffic_light_spacing + 8) * scale_f;
    float x = max((static_cast<float>(size.width) - run.width) / 2, lights_end);
    float baseline = (static_cast<float>(size.height) + m_title_font->cap_height()) / 2;
    m_title_font->draw(painter, { x, baseline }, run, window.active ? m_theme.title_text : m_theme.title_text_inactive);
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
}

void Server::pointer_changed(IntPoint point, u32 buttons)
{
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
    if (value == 1 && handle_hotkey(code))
        return;
    if (!m_active || !m_active->client)
        return;
    char text[2] = {};
    if (value && code < array_size(us_layout)) {
        auto const& pair = us_layout[code];
        bool is_letter = pair.plain >= 'a' && pair.plain <= 'z';
        bool shifted = m_modifiers & Window::ModifierShift;
        bool upper = shifted != (is_letter && (m_modifiers & Window::ModifierCapsLock));
        text[0] = upper ? pair.shifted : pair.plain;
    }
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Key));
    encoder.encode(m_active->id);
    encoder.encode(value != 0);
    encoder.encode(code);
    encoder.encode(m_modifiers);
    encoder.encode(StringView(text));
    send_to(*m_active->client, encoder);
}

}
