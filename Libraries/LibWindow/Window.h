/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibIPC/Listener.h>
#include <LibWindow/Protocol.h>
#include <WindowServerEndpoint.h>

// A program's side of the window server.

namespace Window {

class Window;

// One per program: the link to the server, and its events on the event loop.
class Connection final : public WindowClientStub {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Connection>> connect(Core::EventLoop&);

    [[nodiscard]] WindowServerProxy& server() { return m_server; }
    [[nodiscard]] Gfx::IntSize screen_size() const { return m_screen_size; }
    [[nodiscard]] int scale() const { return m_scale; }
    [[nodiscard]] StringView appearance() const { return m_appearance.view(); }

    Function<void(StringView)> on_appearance_change;
    Function<void(u32)> on_menu_item_chosen;
    Function<void()> on_quit_requested;
    Function<void(StringView)> on_shell_state;
    Function<void(u16, u32)> on_hotkey;
    Function<void(u32)> on_accessibility_requested;

private:
    friend class Window;

    Connection(WindowServerProxy, Gfx::IntSize screen, int scale, String appearance);

    void mouse(IPC::Sender const&, u32 window, u8 kind, i32 x, i32 y, u32 buttons, u32 modifiers, i32 wheel_x, i32 wheel_y) override;
    void moved(IPC::Sender const&, u32 window, i32 x, i32 y) override;
    void appearance_changed(IPC::Sender const&, String name) override;
    void drag(IPC::Sender const&, u32 window, u8 phase, i32 x, i32 y, String type, Vector<u8> data) override;
    void drag_finished(IPC::Sender const&, u32 window, bool accepted) override;
    void menu_item_chosen(IPC::Sender const&, u32 item) override;
    void quit_requested(IPC::Sender const&) override;
    void shell_state(IPC::Sender const&, String json) override;
    void hotkey(IPC::Sender const&, u16 code, u32 modifiers) override;
    void accessibility_requested(IPC::Sender const&, u32 generation) override;
    void key(IPC::Sender const&, u32 window, bool pressed, u16 code, u32 modifiers, String text) override;
    void resized(IPC::Sender const&, u32 window, i32 width, i32 height) override;
    void close_requested(IPC::Sender const&, u32 window) override;
    void activated(IPC::Sender const&, u32 window, bool active) override;
    void presented(IPC::Sender const&, u32 window) override;

    [[nodiscard]] Window* window_with_id(u32);

    WindowServerProxy m_server;
    OwnPtr<IPC::Listener> m_listener;
    Gfx::IntSize m_screen_size;
    int m_scale { 1 };
    String m_appearance;
    Vector<Window*> m_windows;
};

struct MouseEvent {
    MouseKind kind { MouseKind::Moved };
    Gfx::IntPoint position;
    u32 buttons { 0 };
    u32 modifiers { 0 };
    int wheel_x { 0 };
    int wheel_y { 0 };
};

struct KeyEvent {
    bool pressed { false };
    u16 code { 0 };
    u32 modifiers { 0 };
    String text;
};

struct DragEvent {
    DragPhase phase { DragPhase::Moved };
    Gfx::IntPoint position;
    StringView type;
    ReadonlyBytes data;
};

class Window final {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Window>> create(Connection&, StringView title, Gfx::IntRect frame, u32 flags);
    ~Window();

    [[nodiscard]] u32 id() const { return m_id; }
    [[nodiscard]] Gfx::IntSize size() const { return m_size; }
    [[nodiscard]] Gfx::IntPoint position() const { return m_position; }
    [[nodiscard]] Gfx::IntRect frame() const { return { m_position.x, m_position.y, m_size.width, m_size.height }; }
    [[nodiscard]] Connection& connection() { return m_connection; }

    // What to draw on, in pixels: the buffer not on screen. It holds what was
    // last presented.
    [[nodiscard]] Gfx::Bitmap& back_buffer() { return *m_buffers[1 - m_front]; }
    // Shows it, this part having changed (in pixels; whole, without one).
    void present();
    void present(Gfx::IntRect damage);
    // Until what was presented is on screen, so the other buffer is free.
    // (Runs the event loop meanwhile.)
    void wait_until_presented(Core::EventLoop&);

    void set_title(StringView);
    void set_frame(Gfx::IntRect);
    void show(bool visible = true);
    void activate();
    void set_level(int);
    // In points; negative for the theme's.
    void set_corner_radius(int);
    void set_title_bar_height(int);
    void begin_move();
    void grab_pointer(bool);
    void set_cursor(CursorShape);
    void begin_drag(Window& image, StringView type, ReadonlyBytes data);
    void accept_drop(bool);
    void set_input_region(Gfx::IntRect);
    void minimize();
    void zoom();
    struct MaterialRegion {
        Gfx::IntRect rect;
        Material material;
        int radius { 0 };
    };
    void set_materials(Span<MaterialRegion const>);

    Function<void(MouseEvent const&)> on_mouse;
    Function<void(KeyEvent const&)> on_key;
    Function<void(Gfx::IntSize)> on_resize;
    Function<void(Gfx::IntPoint)> on_move;
    Function<void(DragEvent const&)> on_drag;
    Function<void(bool accepted)> on_drag_finished;
    Function<void()> on_close;
    Function<void(bool)> on_activation;
    // What was last presented is on screen: the other buffer is free to draw on.
    Function<void()> on_presented;

private:
    friend class Connection;

    Window(Connection&, u32 id, Gfx::IntRect);
    ErrorOr<void> make_surface(Gfx::IntSize points);
    void release_surface();

    Connection& m_connection;
    u32 m_id;
    Gfx::IntSize m_size;
    Gfx::IntPoint m_position;
    CursorShape m_cursor { CursorShape::Arrow };
    void* m_memory { nullptr };
    size_t m_memory_size { 0 };
    RefPtr<Gfx::Bitmap> m_buffers[2];
    int m_front { 0 };
    bool m_waiting_for_present { false };
};

}
