/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Compositor.h"
#include "Screen.h"
#include "ServerWindow.h"
#include "Theme.h"
#include <LibBase/OwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Listener.h>
#include <LibText/Font.h>
#include <WindowServerEndpoint.h>

namespace WindowServer {

struct Client {
    pid_t pid { 0 };
    IPC::Port events;
    int watch { 0 };
};

// The service: clients and their windows, the window manager (stacking, focus,
// moving and resizing), and input.
class Server final : public WindowServerStub {
public:
    Server(Core::EventLoop&, Screen&, Theme&, Compositor&, Vector<ServerWindow*>& windows, NonnullRefPtr<Text::Typeface> title_face, StringView appearance);

    void watch_input(int fd);
    void set_up_cursor();
    void start();

private:
    // The endpoint.
    WindowServerEndpoint::ConnectAnswer connect(IPC::Sender const&, IPC::Port events) override;
    WindowServerEndpoint::CreateWindowAnswer create_window(IPC::Sender const&, String title, i32 x, i32 y, i32 width, i32 height, u32 flags) override;
    void destroy_window(IPC::Sender const&, u32 window) override;
    void set_title(IPC::Sender const&, u32 window, String title) override;
    void set_frame(IPC::Sender const&, u32 window, i32 x, i32 y, i32 width, i32 height) override;
    void show(IPC::Sender const&, u32 window, bool visible) override;
    void activate(IPC::Sender const&, u32 window) override;
    void set_level(IPC::Sender const&, u32 window, i32 level) override;
    void set_corner_radius(IPC::Sender const&, u32 window, i32 radius) override;
    void set_title_bar(IPC::Sender const&, u32 window, i32 height) override;
    void begin_move(IPC::Sender const&, u32 window) override;
    void grab_pointer(IPC::Sender const&, u32 window, bool grab) override;
    void set_cursor(IPC::Sender const&, u32 window, i32 shape) override;
    i32 set_appearance(IPC::Sender const&, String name) override;
    void begin_drag(IPC::Sender const&, u32 window, u32 image, String type, Vector<u8> data) override;
    void accept_drop(IPC::Sender const&, u32 window, bool accepted) override;
    i32 set_surface(IPC::Sender const&, u32 window, IPC::Port memory, i32 width, i32 height) override;
    void present(IPC::Sender const&, u32 window, u32 buffer, i32 x, i32 y, i32 width, i32 height) override;
    void set_materials(IPC::Sender const&, u32 window, Vector<i32> regions) override;
    void inject_pointer(IPC::Sender const&, i32 x, i32 y, u32 buttons) override;
    WindowServerEndpoint::StatisticsAnswer statistics(IPC::Sender const&) override;
    WindowServerEndpoint::CaptureAnswer capture(IPC::Sender const&) override;

    [[nodiscard]] Client* client_of(IPC::Sender const&);
    [[nodiscard]] ServerWindow* window_of(IPC::Sender const&, u32 id);
    void remove_client(Client&);
    void remove_window(ServerWindow&);
    void release_surface(ServerWindow&);
    void send_to(Client&, IPC::Encoder&);

    // The window manager.
    void activate(ServerWindow*);
    // The frontmost that can be, when the active one has gone away.
    void activate_frontmost();
    void raise(ServerWindow&);
    void insert_by_level(ServerWindow&);
    void send_moved(ServerWindow&);
    void send_resized(ServerWindow&);
    [[nodiscard]] int title_bar_height(ServerWindow const&) const;
    void update_cursor();
    void move_window_to(ServerWindow&, Gfx::IntRect frame);
    [[nodiscard]] ServerWindow* window_at(Gfx::IntPoint);
    [[nodiscard]] WindowButton button_at(ServerWindow const&, Gfx::IntPoint point) const;
    [[nodiscard]] int resize_edges_at(ServerWindow const&, Gfx::IntPoint point) const;
    void render_chrome(ServerWindow&);
    void invalidate_title_bar(ServerWindow&);

    // Input.
    void read_input();
    void pointer_changed(Gfx::IntPoint, u32 buttons);
    void wheel_moved(int x, int y);
    void key_changed(u16 code, int value);
    void drag_pointer_changed(Gfx::IntPoint, u32 released);
    void send_drag(ServerWindow&, Window::DragPhase, Gfx::IntPoint, bool with_data);
    void send_mouse(ServerWindow&, Window::MouseKind, Gfx::IntPoint screen_point, u32 buttons, int wheel_x = 0, int wheel_y = 0);

    void schedule_frame();
    void render_frame();

    Core::EventLoop& m_loop;
    Screen& m_screen;
    Theme& m_theme;
    String m_appearance;
    Compositor& m_compositor;
    Vector<ServerWindow*>& m_windows;
    Vector<NonnullOwnPtr<Client>> m_clients;
    u32 m_next_window_id { 1 };
    ServerWindow* m_active { nullptr };

    Text::GlyphAtlas m_atlas;
    NonnullRefPtr<Text::Typeface> m_title_face;
    OwnPtr<Text::Font> m_title_font;

    int m_input_fd { -1 };
    Gfx::IntPoint m_pointer;
    u32 m_buttons { 0 };
    u32 m_modifiers { 0 };
    int m_pending_absolute_x { -1 };
    int m_pending_absolute_y { -1 };

    enum class Gesture : u8 {
        None,
        Moving,
        Resizing,
        ContentPress,
        ButtonPress,
    };
    Gesture m_gesture { Gesture::None };
    ServerWindow* m_gesture_window { nullptr };
    Gfx::IntPoint m_gesture_start;
    Gfx::IntRect m_gesture_frame;
    int m_gesture_edges { 0 };
    ServerWindow* m_hovered { nullptr };
    ServerWindow* m_grab { nullptr };

    struct Drag {
        ServerWindow* source { nullptr };
        ServerWindow* image { nullptr };
        Gfx::IntPoint image_offset;
        String type;
        Vector<u8> data;
        ServerWindow* target { nullptr };
    };
    OwnPtr<Drag> m_drag;

    Window::CursorShape m_cursor_shape { Window::CursorShape::Arrow };

    bool m_frame_scheduled { false };
    u64 m_next_frame_time { 0 };
};

}
