/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Server.h"
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

Server::Server(Core::EventLoop& loop, Screen& screen, Theme const& theme, Compositor& compositor, Vector<ServerWindow*>& windows, NonnullRefPtr<Text::Typeface> title_face)
    : m_loop(loop)
    , m_screen(screen)
    , m_theme(theme)
    , m_compositor(compositor)
    , m_windows(windows)
    , m_title_face(move(title_face))
{
    m_title_font = adopt_own(*new Text::Font(m_title_face, theme.title_font_size, static_cast<float>(screen.scale()), m_atlas));
    auto points = screen.points();
    m_pointer = { points.width / 2, points.height / 2 };
}

void Server::start()
{
    schedule_frame();
}

void Server::watch_input(int fd)
{
    m_input_fd = fd;
    m_loop.watch(fd, Core::EventLoop::Interest::Read, [this] { read_input(); });
}

void Server::set_up_cursor()
{
    // An original arrow: the shape every pointer has, drawn here at the scale.
    float scale = static_cast<float>(m_screen.scale());
    auto image = MUST(Bitmap::create({ 64, 64 }));
    Painter painter(*image);
    painter.scale(scale, scale);
    painter.translate(1.5f, 1);
    Path arrow;
    arrow.move_to({ 0, 0 });
    arrow.line_to({ 0, 15 });
    arrow.line_to({ 3.6f, 11.6f });
    arrow.line_to({ 6.2f, 17.4f });
    arrow.line_to({ 8.8f, 16.2f });
    arrow.line_to({ 6.2f, 10.6f });
    arrow.line_to({ 11, 10.6f });
    arrow.close();
    StrokeStyle outline;
    outline.width = 1.5f;
    outline.join = LineJoin::Round;
    painter.stroke_path(arrow, outline, Color::from_rgb(0xffffff));
    painter.fill_path(arrow, Color::from_rgb(0x000000));
    m_screen.set_cursor(*image, { static_cast<int>(1.5f * scale), static_cast<int>(scale) });
    m_screen.move_cursor({ m_pointer.x * m_screen.scale(), m_pointer.y * m_screen.scale() });
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
    for (size_t i = m_windows.size(); i > 0; --i) {
        if (m_windows[i - 1]->client == &client)
            remove_window(*m_windows[i - 1]);
    }
    m_loop.unwatch(client.watch);
    m_clients.remove_all_matching([&](auto const& owned) { return owned.ptr() == &client; });
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
    m_windows.remove_all_matching([&](auto* other) { return other == &window; });
    delete &window;
    // The next window down takes over.
    if (!m_active) {
        for (size_t i = m_windows.size(); i > 0; --i) {
            if (m_windows[i - 1]->visible) {
                activate(m_windows[i - 1]);
                break;
            }
        }
    }
}

WindowServerEndpoint::CreateWindowAnswer Server::create_window(IPC::Sender const& sender, String title, i32 x, i32 y, i32 width, i32 height, u32 flags)
{
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
    m_windows.append(window);
    answer.window = window->id;
    if (!(flags & Window::WindowHidden)) {
        window->visible = true;
        activate(window);
        m_compositor.invalidate_window(*window);
    }
    schedule_frame();
    return answer;
}

void Server::destroy_window(IPC::Sender const& sender, u32 id)
{
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

void Server::show(IPC::Sender const& sender, u32 id, bool visible)
{
    auto* window = window_of(sender, id);
    if (!window || window->visible == visible)
        return;
    window->visible = visible;
    m_compositor.invalidate_window(*window);
    if (visible) {
        activate(window);
    } else if (m_active == window) {
        m_active = nullptr;
        for (size_t i = m_windows.size(); i > 0; --i) {
            if (m_windows[i - 1]->visible) {
                activate(m_windows[i - 1]);
                break;
            }
        }
    }
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
    if (!window || regions.size() % 5 != 0)
        return;
    window->materials.clear();
    for (size_t i = 0; i < regions.size(); i += 5) {
        i32 material = regions[i + 4];
        if (material < 0 || material >= static_cast<i32>(Material::Count))
            continue;
        window->materials.append(MaterialRegion { { regions[i], regions[i + 1], regions[i + 2], regions[i + 3] }, static_cast<Material>(material) });
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
        m_active->active = false;
        m_active->chrome_is_stale = true;
        // Its shadow shrinks: what it covered, while it was big, too.
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
    m_windows.append(&window);
    m_compositor.invalidate_window(window);
    schedule_frame();
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
    for (size_t i = m_windows.size(); i > 0; --i) {
        auto* window = m_windows[i - 1];
        if (!window->visible)
            continue;
        auto bounds = window->is_resizable() ? window->frame.inflated(resize_grab) : window->frame;
        if (bounds.contains(point))
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
    float center_y = static_cast<float>(m_theme.title_bar_height) / 2;
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
    m_compositor.invalidate({ frame.x, frame.y, frame.width, m_theme.title_bar_height * m_screen.scale() });
    schedule_frame();
}

void Server::render_chrome(ServerWindow& window)
{
    window.chrome_is_stale = false;
    int scale = m_screen.scale();
    IntSize size { window.frame.width * scale, m_theme.title_bar_height * scale };
    if (!window.chrome || window.chrome->size() != size) {
        auto bitmap = Bitmap::create(size);
        if (bitmap.is_error())
            return;
        window.chrome = bitmap.release_value();
    }
    auto& bitmap = *window.chrome;
    bitmap.clear(window.active ? m_theme.title_bar_background : m_theme.title_bar_background_inactive);
    Painter painter(bitmap);
    auto scale_f = static_cast<float>(scale);
    // The separator under it: one device pixel.
    painter.fill_rect({ 0, static_cast<float>(size.height - 1), static_cast<float>(size.width), 1 }, m_theme.title_bar_separator);

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

    switch (m_gesture) {
    case Gesture::Moving:
        if (m_gesture_window && moved) {
            auto frame = m_gesture_frame.translated(point.x - m_gesture_start.x, point.y - m_gesture_start.y);
            // The title bar stays reachable.
            frame.y = max(frame.y, 0);
            move_window_to(*m_gesture_window, frame);
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
            move_window_to(*m_gesture_window, frame);
            if (m_gesture_window->client) {
                IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Resized));
                encoder.encode(m_gesture_window->id);
                encoder.encode(static_cast<i32>(frame.width));
                encoder.encode(static_cast<i32>(frame.height));
                send_to(*m_gesture_window->client, encoder);
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
                        // Into the Dock, once there is one; out of sight until then.
                        m_gesture_window->visible = false;
                        m_compositor.invalidate_window(*m_gesture_window);
                        m_active = nullptr;
                        for (size_t i = m_windows.size(); i > 0; --i) {
                            if (m_windows[i - 1]->visible) {
                                activate(m_windows[i - 1]);
                                break;
                            }
                        }
                    } else if (button == WindowButton::Zoom) {
                        auto screen_points = m_screen.points();
                        move_window_to(*m_gesture_window, { 0, 0, screen_points.width, screen_points.height });
                        if (m_gesture_window->client) {
                            IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::Resized));
                            encoder.encode(m_gesture_window->id);
                            encoder.encode(static_cast<i32>(screen_points.width));
                            encoder.encode(static_cast<i32>(screen_points.height));
                            send_to(*m_gesture_window->client, encoder);
                        }
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
        activate(window);
        if (window) {
            m_gesture_window = window;
            m_gesture_start = point;
            m_gesture_frame = window->frame;
            auto button = button_at(*window, point);
            int edges = resize_edges_at(*window, point);
            bool in_title_bar = window->is_titled() && point.y < window->frame.y + m_theme.title_bar_height;
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
            float group_top = top + (static_cast<float>(m_theme.title_bar_height) - diameter) / 2;
            bool over = static_cast<float>(point.x) >= static_cast<float>(window->frame.x) + m_theme.traffic_light_inset
                && static_cast<float>(point.x) < group_right && static_cast<float>(point.y) >= group_top && static_cast<float>(point.y) < group_top + diameter;
            if (over != window->lights_hovered) {
                window->lights_hovered = over;
                invalidate_title_bar(*window);
            }
        }
    }
    schedule_frame();
}

void Server::wheel_moved(int x, int y)
{
    if (auto* window = window_at(m_pointer))
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
