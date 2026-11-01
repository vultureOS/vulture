/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <LibWindow/Window.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/vulture.h>
#include <unistd.h>

namespace Window {

Connection::Connection(WindowServerProxy server, Gfx::IntSize screen, int scale, String appearance)
    : m_server(move(server))
    , m_screen_size(screen)
    , m_scale(scale)
    , m_appearance(move(appearance))
{
}

ErrorOr<NonnullOwnPtr<Connection>> Connection::connect(Core::EventLoop& loop)
{
    auto port = TRY(IPC::look_up("com.vulture.windowserver"_sv));
    WindowServerProxy server(move(port));
    auto events = TRY(IPC::create_port());
    auto answer = TRY(server.connect(move(events.send)));
    if (answer.error)
        return Error::from_errno(answer.error);
    auto* connection = new (Base::nothrow) Connection(move(server), { answer.width, answer.height }, answer.scale, move(answer.appearance));
    if (!connection)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*connection);
    auto* listener = new (Base::nothrow) IPC::Listener(loop, move(events.receive), [connection](IPC::ReceivedMessage& message) { (void)connection->handle(message); });
    if (!listener)
        return Error::from_errno(ENOMEM);
    owned->m_listener = adopt_own(*listener);
    return owned;
}

Window* Connection::window_with_id(u32 id)
{
    for (auto* window : m_windows) {
        if (window->id() == id)
            return window;
    }
    return nullptr;
}

void Connection::mouse(IPC::Sender const&, u32 id, u8 kind, i32 x, i32 y, u32 buttons, u32 modifiers, i32 wheel_x, i32 wheel_y)
{
    if (auto* window = window_with_id(id); window && window->on_mouse)
        window->on_mouse({ static_cast<MouseKind>(kind), { x, y }, buttons, modifiers, wheel_x, wheel_y });
}

void Connection::moved(IPC::Sender const&, u32 id, i32 x, i32 y)
{
    auto* window = window_with_id(id);
    if (!window)
        return;
    window->m_position = { x, y };
    if (window->on_move)
        window->on_move({ x, y });
}

void Connection::appearance_changed(IPC::Sender const&, String name)
{
    m_appearance = move(name);
    if (on_appearance_change)
        on_appearance_change(m_appearance.view());
}

void Connection::drag(IPC::Sender const&, u32 id, u8 phase, i32 x, i32 y, String type, Vector<u8> data)
{
    if (auto* window = window_with_id(id); window && window->on_drag)
        window->on_drag({ static_cast<DragPhase>(phase), { x, y }, type.view(), data.span() });
}

void Connection::drag_finished(IPC::Sender const&, u32 id, bool accepted)
{
    if (auto* window = window_with_id(id); window && window->on_drag_finished)
        window->on_drag_finished(accepted);
}

void Connection::menu_item_chosen(IPC::Sender const&, u32 item)
{
    if (on_menu_item_chosen)
        on_menu_item_chosen(item);
}

void Connection::quit_requested(IPC::Sender const&)
{
    if (on_quit_requested)
        on_quit_requested();
}

void Connection::shell_state(IPC::Sender const&, String json)
{
    if (on_shell_state)
        on_shell_state(json.view());
}

void Connection::hotkey(IPC::Sender const&, u16 code, u32 modifiers)
{
    if (on_hotkey)
        on_hotkey(code, modifiers);
}

void Connection::key(IPC::Sender const&, u32 id, bool pressed, u16 code, u32 modifiers, String text)
{
    if (auto* window = window_with_id(id); window && window->on_key)
        window->on_key({ pressed, code, modifiers, move(text) });
}

void Connection::resized(IPC::Sender const&, u32 id, i32 width, i32 height)
{
    auto* window = window_with_id(id);
    if (!window)
        return;
    window->m_size = { width, height };
    window->release_surface();
    if (window->make_surface({ width, height }).is_error())
        return;
    if (window->on_resize)
        window->on_resize({ width, height });
}

void Connection::close_requested(IPC::Sender const&, u32 id)
{
    if (auto* window = window_with_id(id); window && window->on_close)
        window->on_close();
}

void Connection::activated(IPC::Sender const&, u32 id, bool active)
{
    if (auto* window = window_with_id(id); window && window->on_activation)
        window->on_activation(active);
}

void Connection::presented(IPC::Sender const&, u32 id)
{
    if (auto* window = window_with_id(id)) {
        window->m_waiting_for_present = false;
        if (window->on_presented)
            window->on_presented();
    }
}

Window::Window(Connection& connection, u32 id, Gfx::IntRect frame)
    : m_connection(connection)
    , m_id(id)
    , m_size(frame.size())
    , m_position(frame.origin())
{
}

ErrorOr<NonnullOwnPtr<Window>> Window::create(Connection& connection, StringView title, Gfx::IntRect frame, u32 flags)
{
    auto answer = TRY(connection.server().create_window(title, frame.x, frame.y, frame.width, frame.height, flags));
    if (answer.error)
        return Error::from_errno(answer.error);
    auto* window = new (Base::nothrow) Window(connection, answer.window, frame);
    if (!window)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*window);
    connection.m_windows.append(window);
    TRY(owned->make_surface(frame.size()));
    return owned;
}

Window::~Window()
{
    m_connection.m_windows.remove_all_matching([this](auto* window) { return window == this; });
    (void)m_connection.server().destroy_window(m_id);
    release_surface();
}

ErrorOr<void> Window::make_surface(Gfx::IntSize points)
{
    int scale = m_connection.scale();
    Gfx::IntSize pixels { points.width * scale, points.height * scale };
    size_t buffer_size = static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height) * sizeof(Gfx::Pixel);
    size_t total = buffer_size * 2;
    int fd = vulture_shared_memory_create(total, O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    IPC::Port memory(fd);
    void* mapped = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED)
        return Error::from_errno(errno);
    auto* first = static_cast<Gfx::Pixel*>(mapped);
    m_memory = mapped;
    m_memory_size = total;
    m_buffers[0] = TRY(Gfx::Bitmap::wrap(first, pixels, pixels.width));
    m_buffers[1] = TRY(Gfx::Bitmap::wrap(first + static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height), pixels, pixels.width));
    m_front = 0;
    m_waiting_for_present = false;
    i32 error = TRY(m_connection.server().set_surface(m_id, move(memory), pixels.width, pixels.height));
    if (error)
        return Error::from_errno(error);
    return {};
}

void Window::release_surface()
{
    m_buffers[0] = nullptr;
    m_buffers[1] = nullptr;
    if (m_memory)
        munmap(m_memory, m_memory_size);
    m_memory = nullptr;
    m_memory_size = 0;
}

void Window::present()
{
    if (m_buffers[0])
        present(m_buffers[0]->rect());
}

void Window::present(Gfx::IntRect damage)
{
    if (!m_buffers[0])
        return;
    int back = 1 - m_front;
    m_waiting_for_present = true;
    (void)m_connection.server().present(m_id, static_cast<u32>(back), damage.x, damage.y, damage.width, damage.height);
    // The other buffer gets what this one has, so drawing goes on from it.
    auto const& shown = *m_buffers[back];
    auto& next = *m_buffers[m_front];
    auto area = damage.intersected(shown.rect());
    for (int y = area.y; y < area.bottom(); ++y)
        memcpy(next.scanline(y) + area.x, shown.scanline(y) + area.x, static_cast<size_t>(area.width) * sizeof(Gfx::Pixel));
    m_front = back;
}

void Window::wait_until_presented(Core::EventLoop& loop)
{
    while (m_waiting_for_present)
        loop.pump();
}

void Window::set_title(StringView title)
{
    (void)m_connection.server().set_title(m_id, title);
}

void Window::set_frame(Gfx::IntRect frame)
{
    (void)m_connection.server().set_frame(m_id, frame.x, frame.y, frame.width, frame.height);
    m_position = frame.origin();
    if (frame.size() == m_size)
        return;
    // Pixels for the new size, as when the window manager resizes it.
    m_size = frame.size();
    release_surface();
    (void)make_surface(m_size);
}

void Window::activate()
{
    (void)m_connection.server().activate(m_id);
}

void Window::set_level(int level)
{
    (void)m_connection.server().set_level(m_id, level);
}

void Window::set_corner_radius(int radius)
{
    (void)m_connection.server().set_corner_radius(m_id, radius);
}

void Window::set_title_bar_height(int height)
{
    (void)m_connection.server().set_title_bar(m_id, height);
}

void Window::set_input_region(Gfx::IntRect region)
{
    (void)m_connection.server().set_input_region(m_id, region.x, region.y, region.width, region.height);
}

void Window::minimize()
{
    (void)m_connection.server().minimize(m_id);
}

void Window::zoom()
{
    (void)m_connection.server().zoom(m_id);
}

void Window::begin_move()
{
    (void)m_connection.server().begin_move(m_id);
}

void Window::grab_pointer(bool grab)
{
    (void)m_connection.server().grab_pointer(m_id, grab);
}

void Window::set_cursor(CursorShape shape)
{
    if (shape == m_cursor)
        return;
    m_cursor = shape;
    (void)m_connection.server().set_cursor(m_id, static_cast<i32>(shape));
}

void Window::begin_drag(Window& image, StringView type, ReadonlyBytes data)
{
    (void)m_connection.server().begin_drag(m_id, image.id(), type, data);
}

void Window::accept_drop(bool accepted)
{
    (void)m_connection.server().accept_drop(m_id, accepted);
}

void Window::show(bool visible)
{
    (void)m_connection.server().show(m_id, visible);
}

void Window::set_materials(Span<MaterialRegion const> regions)
{
    Vector<i32> flat;
    for (size_t i = 0; i < regions.size(); ++i) {
        flat.append(regions[i].rect.x);
        flat.append(regions[i].rect.y);
        flat.append(regions[i].rect.width);
        flat.append(regions[i].rect.height);
        flat.append(static_cast<i32>(regions[i].material));
        flat.append(regions[i].radius);
    }
    (void)m_connection.server().set_materials(m_id, flat);
}

}
