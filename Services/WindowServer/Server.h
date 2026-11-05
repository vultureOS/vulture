/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "KeyboardLayout.h"
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
    // An application, if it said so: the Dock and menu bar show those.
    String application_name;
    String application_identifier;
    String menus;
    bool hidden { false };
    // Hears of everything (the Dock, the menu bar).
    bool is_shell { false };
    // Whether it's there: what it's last been asked, when, and if it has gone too long without saying.
    u32 ping_serial { 0 };
    u64 ping_sent_at { 0 };
    u64 answered_at { 0 };
    bool not_responding { false };
    // Its last answer to request_accessibility, and to which.
    u32 accessibility_generation { 0 };
    String accessibility;
};

// The service: clients and their windows, the window manager (stacking, focus,
// moving and resizing), and input.
class Server final : public WindowServerStub {
public:
    Server(Core::EventLoop&, Screen&, Theme&, Compositor&, Vector<ServerWindow*>& windows, NonnullRefPtr<Text::Typeface> title_face, StringView appearance);

    // A progress bar on an empty screen until the first window of the session shows.
    void show_boot_screen();

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
    i32 set_keyboard_layout(IPC::Sender const&, String identifier) override;
    void begin_drag(IPC::Sender const&, u32 window, u32 image, String type, Vector<u8> data) override;
    void accept_drop(IPC::Sender const&, u32 window, bool accepted) override;
    i32 set_surface(IPC::Sender const&, u32 window, IPC::Port memory, i32 width, i32 height) override;
    void present(IPC::Sender const&, u32 window, u32 buffer, i32 x, i32 y, i32 width, i32 height) override;
    void set_materials(IPC::Sender const&, u32 window, Vector<i32> regions) override;
    WindowServerEndpoint::CreateLayerAnswer create_layer(IPC::Sender const&, u32 window, u32 parent) override;
    i32 set_layer_surface(IPC::Sender const&, u32 window, u32 layer, IPC::Port memory, i32 width, i32 height) override;
    void commit_layers(IPC::Sender const&, u32 window, u32 serial, Vector<u8> changes) override;
    WindowServerEndpoint::LayerFrameLogAnswer layer_frame_log(IPC::Sender const&, u32 window, u32 layer) override;
    void set_application(IPC::Sender const&, String name, String identifier) override;
    void set_menus(IPC::Sender const&, String menus) override;
    void subscribe_shell(IPC::Sender const&) override;
    void choose_menu_item(IPC::Sender const&, i32 pid, u32 item) override;
    void activate_application(IPC::Sender const&, i32 pid) override;
    void hide_application(IPC::Sender const&, i32 pid, bool hidden) override;
    void quit_application(IPC::Sender const&, i32 pid) override;
    void activate_window(IPC::Sender const&, u32 window) override;
    void minimize(IPC::Sender const&, u32 window) override;
    void zoom(IPC::Sender const&, u32 window) override;
    void set_minimize_target(IPC::Sender const&, i32 x, i32 y) override;
    void set_work_area(IPC::Sender const&, i32 top, i32 left, i32 bottom, i32 right) override;
    void register_hotkey(IPC::Sender const&, u16 code, u32 modifiers) override;
    i32 set_wallpaper(IPC::Sender const&, String name) override;
    void set_overview(IPC::Sender const&, bool shown) override;
    void set_input_region(IPC::Sender const&, u32 window, i32 x, i32 y, i32 width, i32 height) override;
    void switch_space(IPC::Sender const&, i32 space) override;
    void inject_pointer(IPC::Sender const&, i32 x, i32 y, u32 buttons, u64 sent_at) override;
    WindowServerEndpoint::StatisticsAnswer statistics(IPC::Sender const&) override;
    void damage_everything(IPC::Sender const&) override;
    WindowServerEndpoint::CaptureAnswer capture(IPC::Sender const&) override;
    WindowServerEndpoint::InputLatencyAnswer input_latency(IPC::Sender const&) override;
    void pong(IPC::Sender const&, u32 serial) override;
    WindowServerEndpoint::CursorShapeAnswer cursor_shape(IPC::Sender const&) override;
    WindowServerEndpoint::RequestAccessibilityAnswer request_accessibility(IPC::Sender const&) override;
    void publish_accessibility(IPC::Sender const&, u32 generation, String json) override;
    WindowServerEndpoint::AccessibilityAnswer accessibility(IPC::Sender const&, u32 generation) override;

    [[nodiscard]] Client* client_of(IPC::Sender const&);
    [[nodiscard]] ServerWindow* window_of(IPC::Sender const&, u32 id);
    void remove_client(Client&);
    void remove_window(ServerWindow&);
    void release_surface(ServerWindow&);
    void send_to(Client&, IPC::Encoder&);
    // It's been sent input: is it there to get it?
    void ask_if_responding(Client&);
    void check_who_is_responding();

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

    // The desktop: Shell.cpp.
    [[nodiscard]] Client* client_with_pid(i32 pid);
    void update_visibility(ServerWindow&);
    void update_all_visibility();
    void shell_state_changed();
    void send_shell_state();
    void minimize_window(ServerWindow&);
    void restore_window(ServerWindow&);
    void zoom_window(ServerWindow&);
    void activate_client(Client&);
    void cycle_applications(bool backward);
    [[nodiscard]] bool handle_hotkey(u16 code);
    void go_to_space(int space, bool animated);
    void enter_overview();
    void leave_overview(ServerWindow* to_activate);
    void layout_overview();
    void update_overview_overlays();
    [[nodiscard]] bool overview_pointer(Gfx::IntPoint, u32 pressed);
    void start_animating();
    void animation_tick();
    void apply_wallpaper();
    void update_boot_overlays();

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

    // Layers.
    void apply_layer_change(ServerWindow&, Window::LayerChange const&, double now);
    void destroy_layer(ServerWindow&, Layer&);
    // Each to where it should be by this time. Whether anything's still on its way.
    [[nodiscard]] bool advance_layers(double now);
    void log_layers(double now);

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
    KeyboardLayout m_keyboard_layout { KeyboardLayout::builtin() };
    u16 m_repeating_key { 0 };
    int m_key_repeat_timer { -1 };
    void start_key_repeat(u16 code);
    void stop_key_repeat();
    void deliver_key(u16 code, int value);
    int m_pending_absolute_x { -1 };
    int m_pending_relative_x { 0 };
    int m_pending_relative_y { 0 };
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
    int m_cursor_turn { 0 };
    int m_cursor_timer { -1 };
    int m_responding_timer { -1 };

    bool m_frame_scheduled { false };
    int m_frame_timer { -1 };
    u64 m_frame_due { 0 };
    u64 m_next_frame_time { 0 };
    u64 m_last_frame_started { 0 };
    // Someone's waiting to see what they just did.
    bool m_input_waiting { false };
    // A layer's on its way somewhere, so there are frames to come.
    bool m_layers_animating { false };
    u64 m_last_input_at { 0 };
    void note_input();

    // The desktop.
    Client* m_active_application { nullptr };
    bool m_shell_state_pending { false };
    u32 m_accessibility_generation { 0 };
    // For input_latency: when the pointer last moved, whether that moved a window not yet on the screen, and how long
    // those took, in half-milliseconds, the last for anything longer.
    u64 m_pointer_moved_at { 0 };
    u64 m_event_time { 0 };
    u64 m_latency_started { 0 };
    static constexpr size_t latency_buckets = 256;
    u64 m_latency_counts[latency_buckets] {};
    u64 m_latency_worst { 0 };
    Gfx::IntPoint m_minimize_target { 0, 0 };
    int m_work_top { 0 };
    int m_work_left { 0 };
    int m_work_bottom { 0 };
    int m_work_right { 0 };
    String m_wallpaper_name;
    struct Hotkey {
        u16 code;
        u32 modifiers;
        Client* client;
    };
    Vector<Hotkey> m_hotkeys;

    struct Genie {
        ServerWindow* window { nullptr };
        RefPtr<Gfx::Bitmap> picture;
        Gfx::FloatRect from;
        Gfx::FloatPoint to;
        double started { 0 };
        // Backwards: out of the Dock.
        bool restoring { false };
    };
    Vector<Genie> m_genies;
    int m_animation_timer { -1 };

    int m_space { 0 };
    int m_space_count { 1 };
    // A slide between spaces.
    bool m_sliding { false };
    int m_slide_from { 0 };
    double m_slide_started { 0 };

    struct Thumbnail {
        ServerWindow* window { nullptr };
        RefPtr<Gfx::Bitmap> picture;
        Gfx::FloatRect home;
        Gfx::FloatRect spread;
    };
    Vector<Thumbnail> m_thumbnails;
    bool m_overview { false };
    bool m_overview_leaving { false };
    ServerWindow* m_overview_chosen { nullptr };
    double m_overview_started { 0 };
    int m_overview_hover { -1 };
    int m_overview_hover_space { -1 };

    bool m_boot_screen { false };
    double m_boot_started { 0 };
    bool m_boot_finishing { false };
    double m_boot_finished_at { 0 };
};

}
