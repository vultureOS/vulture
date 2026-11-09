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
#include <LibGesture/Recognizer.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <LibText/Font.h>
#include <WindowServerEndpoint.h>

namespace WindowServer {

// The desktop's own window, and what sits on it (widgets): under every other, and gone where a Space is full screen.
[[nodiscard]] constexpr bool is_desktop_level(i32 level)
{
    return level >= Window::LevelDesktop && level < Window::LevelNormal;
}

struct Client {
    pid_t pid { 0 };
    // Whose it is: a session's own windows are only shown while it's on the screen.
    uid_t uid { 0 };
    IPC::Port events;
    int watch { 0 };
    // An application, if it said so: the Dock and menu bar show those.
    String application_name;
    String application_identifier;
    // On its icon in the Dock.
    String badge;
    // What it keeps clear of windows at each edge (the menu bar's, the Dock's): top, left, bottom, right.
    int work_area_reserved[4] {};
    String menus;
    bool hidden { false };
    // Hears of everything (the Dock, the menu bar).
    bool is_shell { false };
    // Whether it's there: what it's last been asked, when, and if it has gone too long without saying.
    u32 ping_serial { 0 };
    u64 ping_sent_at { 0 };
    u64 answered_at { 0 };
    bool not_responding { false };
    bool shows_window_menus { false };
    // Its last answer to request_accessibility, and to which.
    u32 accessibility_generation { 0 };
    String accessibility;
};

// A desktop's worth of windows, on a display.
struct Space {
    int id { 0 };
    // The window that has it to itself, and if it's Split View, the one beside that.
    ServerWindow* full { nullptr };
    ServerWindow* beside { nullptr };
    bool full_on_left { true };
    float divider { 0.5f };
    // Split View, waiting to be told what goes on the other side.
    bool choosing { false };
    // Done with, once it's stopped being looked at.
    bool leaving { false };
};

struct DisplaySpaces {
    int output { 0 };
    Vector<Space> spaces;
    int current { 0 };
    // Its row of Spaces in Overview: open (pictures of them) or not (names), and how far between.
    bool bar_open { false };
    float bar_amount { 0 };
    float bar_from { 0 };
    double bar_started { 0 };
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
    void set_dock_edge(IPC::Sender const&, i32 edge, i32 thickness) override;
    void set_badge(IPC::Sender const&, String text) override;
    void show_windows_of(IPC::Sender const&, i32 pid) override;
    void add_menu_extra(IPC::Sender const&, u32 window) override;
    void set_menu_extras_edge(IPC::Sender const&, i32 x, i32 gap) override;
    void layout_menu_extras();
    void set_work_area(IPC::Sender const&, i32 top, i32 left, i32 bottom, i32 right) override;
    void register_hotkey(IPC::Sender const&, u16 code, u32 modifiers) override;
    i32 set_wallpaper(IPC::Sender const&, String name) override;
    void set_overview(IPC::Sender const&, bool shown) override;
    void set_session(IPC::Sender const&, u32 uid, bool locked) override;
    u64 idle_time(IPC::Sender const&) override;
    void set_reduce_motion(IPC::Sender const&, bool reduced) override;
    void set_natural_scrolling(IPC::Sender const&, bool natural) override;
    void set_tap_to_click(IPC::Sender const&, bool tap) override;
    void set_pointer_speed(IPC::Sender const&, i32 percent) override;
    WindowServerEndpoint::DisplaysAnswer displays(IPC::Sender const&) override;
    i32 set_display(IPC::Sender const&, i32 output, i32 x, i32 y, i32 width, i32 height) override;
    i32 set_mirroring(IPC::Sender const&, bool mirrored) override;
    i32 set_warmth(IPC::Sender const&, i32 percent) override;
    void set_input_region(IPC::Sender const&, u32 window, i32 x, i32 y, i32 width, i32 height) override;
    void arrange_window(IPC::Sender const&, u32 window, i32 arrangement) override;
    void set_full_screen(IPC::Sender const&, u32 window, bool full) override;
    void toggle_full_screen(IPC::Sender const&, u32 window) override;
    void tile_in_split_view(IPC::Sender const&, u32 window, bool on_left) override;
    void set_hot_corners(IPC::Sender const&, i32 top_left, i32 top_right, i32 bottom_left, i32 bottom_right) override;
    void set_tiling(IPC::Sender const&, bool by_edges, bool to_menu_bar, bool with_option, bool margins) override;
    void take_place_of(IPC::Sender const&, u32 window, u32 other) override;
    void set_click_wallpaper(IPC::Sender const&, bool reveals) override;
    void wallpaper_clicked(IPC::Sender const&) override;
    // The wallpaper pressed, at this point: what's on the desktop comes out from under everything, or goes back.
    void press_on_wallpaper(Gfx::IntPoint, bool activating);
    void set_stage(IPC::Sender const&, bool on) override;
    void hide_others(IPC::Sender const&, bool hidden) override;
    void show_window_menus(IPC::Sender const&) override;
    void switch_space(IPC::Sender const&, i32 space) override;
    void inject_pointer(IPC::Sender const&, i32 x, i32 y, u32 buttons, u64 sent_at) override;
    WindowServerEndpoint::StatisticsAnswer statistics(IPC::Sender const&) override;
    void damage_everything(IPC::Sender const&) override;
    WindowServerEndpoint::CaptureAnswer capture(IPC::Sender const&) override;
    WindowServerEndpoint::CaptureAreaAnswer capture_area(IPC::Sender const&, i32 x, i32 y, i32 width, i32 height) override;
    WindowServerEndpoint::CaptureWindowAnswer capture_window(IPC::Sender const&, u32 window) override;
    WindowServerEndpoint::WindowUnderAnswer window_under(IPC::Sender const&, i32 x, i32 y) override;
    i32 start_recording(IPC::Sender const&, IPC::Port memory, i32 x, i32 y, i32 width, i32 height) override;
    void recording_ready(IPC::Sender const&) override;
    void stop_recording(IPC::Sender const&) override;
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
    // Another of this application's windows first, if it has any: a window going away doesn't take the keyboard from it.
    void activate_frontmost(Client* preferred = nullptr);
    void raise(ServerWindow&);
    void insert_by_level(ServerWindow&);
    void send_moved(ServerWindow&);
    void send_resized(ServerWindow&);
    [[nodiscard]] int title_bar_height(ServerWindow const&) const;
    void update_cursor();
    void move_window_to(ServerWindow&, Gfx::IntRect frame);
    // Whether this one may see what's on the screen, which is logged if not.
    [[nodiscard]] bool may_look_at_screen(IPC::Sender const&, char const* doing_what);
    [[nodiscard]] ErrorOr<Vector<u8>> png_of_area(Gfx::IntRect points);
    void record_frame();
    void end_recording();
    // Everything that follows from the displays having changed.
    void displays_changed();
    // What's free of a display for windows, in points.
    [[nodiscard]] Gfx::IntRect work_area_of(Display const&) const;
    // By hand: when its contents are ready.
    void resize_live(ServerWindow&, Gfx::IntRect frame);
    void finish_live_resize(ServerWindow&);
    void check_live_resizes();
    void show_waiting_surface(ServerWindow&);
    [[nodiscard]] ServerWindow* window_at(Gfx::IntPoint);
    [[nodiscard]] WindowButton button_at(ServerWindow const&, Gfx::IntPoint point) const;
    [[nodiscard]] int resize_edges_at(ServerWindow const&, Gfx::IntPoint point) const;
    void render_chrome(ServerWindow&);
    void invalidate_title_bar(ServerWindow&);

    // The desktop: Shell.cpp.
    [[nodiscard]] Client* client_with_pid(i32 pid);
    void update_visibility(ServerWindow&);
    void update_all_visibility();
    // Whether this client's windows may be on the screen, and have input, with the session as it is.
    [[nodiscard]] bool is_in_session(Client const*) const;
    void recompute_work_area();
    void shell_state_changed();
    void send_shell_state();
    [[nodiscard]] String shell_state_for(uid_t);
    void minimize_window(ServerWindow&);
    void restore_window(ServerWindow&);
    void zoom_window(ServerWindow&);
    void activate_client(Client&);
    void cycle_applications(bool backward);
    void cycle_windows(bool backward);
    // Tiling.
    [[nodiscard]] Gfx::IntRect frame_for_arrangement(ServerWindow const&, Window::WindowArrangement) const;
    void arrange(ServerWindow&, Window::WindowArrangement);
    // Those that place more than one window: where each goes, this one's first. Empty for the others.
    [[nodiscard]] static Vector<Window::WindowArrangement> places_of(Window::WindowArrangement);
    // Where a window would be tiled, if it were let go with the pointer here.
    [[nodiscard]] int tile_zone_at(Gfx::IntPoint, ServerWindow const&) const;
    void show_tile_preview(int arrangement, ServerWindow const&);
    void find_tile_neighbors(ServerWindow&, int edges);
    void arm_window_menu(ServerWindow&);
    void disarm_window_menu();
    [[nodiscard]] bool handle_hotkey(u16 code);
    // Spaces.cpp.
    enum class OverviewMode : u8 {
        Everything,
        OneApplication,
        ChoosingSplit,
    };
    void sync_display_spaces();
    [[nodiscard]] DisplaySpaces const& spaces_of_const(Display const&) const;
    [[nodiscard]] String space_label(DisplaySpaces const&, int index) const;
    // What Overview shows on this display, as the accessibility tree has it (as one of an application's windows).
    [[nodiscard]] JSON::Value overview_accessibility(Display const&) const;
    [[nodiscard]] DisplaySpaces& spaces_of(Display const&);
    // Of the display the pointer's on.
    [[nodiscard]] DisplaySpaces& focused_spaces();
    [[nodiscard]] Display const& display_of_spaces(DisplaySpaces const&) const;
    [[nodiscard]] Space* space_with_id(int id, DisplaySpaces** where = nullptr);
    [[nodiscard]] bool space_is_showing(int id) const;
    [[nodiscard]] bool space_is_sliding(int id) const;
    [[nodiscard]] int index_of_space(int id) const;
    [[nodiscard]] int space_for_new_window(ServerWindow const&);
    // It's been moved on to another display: and so on to what's showing there.
    void settle_on_display(ServerWindow&);
    void show_space_of(ServerWindow const&);
    void go_to_space(DisplaySpaces&, int index, bool animated);
    void finish_slide();
    [[nodiscard]] bool advance_slide(double now);
    void remove_leaving_spaces();
    [[nodiscard]] int hidden_top_of(ServerWindow const&) const;
    void layout_space(Space&, Display const&);
    void enter_full_screen(ServerWindow&);
    void enter_split_view(ServerWindow&, bool on_left);
    void finish_split_view(ServerWindow& chosen);
    void come_back_from_full_screen(ServerWindow&);
    void leave_full_screen(ServerWindow&, bool window_is_going);
    void notify_full_screen(ServerWindow&);
    // The Dock and a menu bar, on a display with a Space that's full screen.
    [[nodiscard]] bool is_kept_out_of_the_way(ServerWindow const&) const;
    [[nodiscard]] Optional<int> split_divider_at(Gfx::IntPoint);
    void drag_divider(Gfx::IntPoint);
    void check_full_screen_edges(Gfx::IntPoint);
    void set_menu_reveal(int output, bool wanted);
    void set_dock_reveal(bool wanted);
    [[nodiscard]] bool advance_reveals(double now);
    void reveal_desktop(bool revealed, bool animated);
    [[nodiscard]] bool advance_desktop_reveal(double now);
    void check_hot_corner(Gfx::IntPoint);
    void fire_hot_corner(int action);
    [[nodiscard]] Vector<Client*> applications_most_recent_first();
    void finish_switching(bool go);
    // Who was before. After m_active_application's been changed.
    void application_became_active(Client* previous);
    void add_to_stage_strip(Client&);
    void layout_stage_strip();
    [[nodiscard]] bool stage_strip_is_covered() const;
    void make_room_for_stage();
    void stage_application_changed();
    [[nodiscard]] bool stage_strip_pointer(Gfx::IntPoint);
    // What's there whatever else is going on.
    [[nodiscard]] Vector<Compositor::Overlay> lasting_overlays();
    void refresh_lasting_overlays();
    [[nodiscard]] Gfx::FloatRect overview_tile(Display const&, DisplaySpaces const&, int index) const;
    void collect_overview_windows();
    void enter_overview(OverviewMode = OverviewMode::Everything);
    void leave_overview(ServerWindow* to_activate);
    void finish_overview_at_once();
    void layout_overview();
    void update_overview_overlays();
    void move_window_to_space(ServerWindow&, DisplaySpaces&, int index);
    void remove_space(DisplaySpaces&, int index);
    [[nodiscard]] bool overview_pointer(Gfx::IntPoint, u32 pressed, u32 released);
    void start_animating();
    void animation_tick();
    void apply_wallpaper();
    // For a wallpaper that changes through the day: which part of the day it is, by the hour, (-1 for one that doesn't).
    int m_wallpaper_period { -1 };
    int m_wallpaper_timer { -1 };

public:
    static void set_still(bool);

private:
    void update_boot_overlays();

    // Input.
    void read_input();
    void pointer_changed(Gfx::IntPoint, u32 buttons);
    void wheel_moved(int x, int y);
    void handle_gesture(::Gesture::Event const&);
    void swipe_changed(float across, float speed, bool ended);
    void touch_event_read(u16 code, int value);
    void touches_finished();
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
    OwnPtr<Text::Font> m_small_label_font;

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
    int m_pending_head { 0 };
    ::Gesture::Recognizer m_gestures;
    // Fingers as they stand, and which have changed since the device last said it was finished saying so.
    struct Touch {
        int x { 0 };
        int y { 0 };
        bool down { false };
        bool was_down { false };
        bool moved { false };
    };
    static constexpr int touch_slots = 10;
    Touch m_touches[touch_slots];
    int m_touch_slot { 0 };
    bool m_natural_scrolling { true };
    bool m_tap_to_click { true };
    int m_pointer_speed { 100 };
    // Following the fingers from one Space to another, how far along.
    bool m_swiping { false };
    float m_swipe_travel { 0 };
    float m_slide_start_travel { 0 };

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
    // Where a window being moved was before it was tiled, if it was, or else where it started: for Return to Previous Size.
    Gfx::IntRect m_gesture_restore;
    int m_gesture_edges { 0 };
    bool m_tile_by_edges { true };
    bool m_tile_to_menu_bar { true };
    bool m_tile_with_option { true };
    bool m_tile_margins { true };
    // Being dragged, where it would be tiled if it were let go, or -1.
    int m_tile_zone { -1 };
    // Windows tiled next to one being resized, that go with it.
    struct TileNeighbor {
        ServerWindow* window;
        Gfx::IntRect start;
        int edges;
    };
    Vector<TileNeighbor> m_tile_neighbors;
    // The green button's menu, waiting for the pointer to stay long enough.
    int m_window_menu_timer { -1 };
    ServerWindow* m_window_menu_window { nullptr };
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
    // Who's logged in, as far as it goes: whoever's desktop this is.
    uid_t m_shell_uid { 0 };
    bool m_has_shell_uid { false };
    // Whose session is on the screen, and whether it's locked: see set_session. Until the login window has said, everyone's.
    bool m_sessions_managed { false };
    uid_t m_session_uid { 0 };
    bool m_session_locked { false };
    // When a key, the pointer or a finger was last used, on the monotonic clock: for idle_time.
    u64 m_last_activity_at { 0 };
    // One at a time, and a frame at a time.
    struct Recording {
        Client* client { nullptr };
        void* memory { nullptr };
        size_t memory_size { 0 };
        Gfx::IntRect points;
        bool ready { true };
        bool new_since { true };
    };
    Recording m_recording;
    int m_cursor_timer { -1 };
    int m_responding_timer { -1 };
    int m_resize_timer { -1 };
    u64 m_resize_mismatches { 0 };
    bool m_reduce_motion { false };

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
    // Menu extras, in the order they came, and where they go up to.
    Vector<ServerWindow*> m_menu_extras;
    int m_menu_extras_edge { 0 };
    int m_menu_extras_gap { 0 };
    // Where the Dock is, and how far it comes out.
    int m_dock_edge { 0 };
    int m_dock_thickness { 0 };
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

    Vector<DisplaySpaces> m_display_spaces;
    int m_next_space_id { 1 };
    // A slide between Spaces: on one display at a time.
    bool m_sliding { false };
    int m_slide_output { 0 };
    int m_slide_from { 0 };
    double m_slide_started { 0 };

    struct Thumbnail {
        ServerWindow* window { nullptr };
        RefPtr<Gfx::Bitmap> picture;
        Gfx::FloatRect home;
        Gfx::FloatRect spread;
        // Full screen: as it is in the row along the top, where a window's corners would be the wrong size.
        RefPtr<Gfx::Bitmap> tile_picture;
        // On a Space that's showing: the others are only in the row along the top.
        bool shown { true };
    };
    struct OverviewWallpaper {
        int output { 0 };
        RefPtr<Gfx::Bitmap> picture;
    };
    Vector<OverviewWallpaper> m_overview_wallpapers;
    OverviewMode m_overview_mode { OverviewMode::Everything };
    Gfx::IntPoint m_overview_press_at;
    int m_overview_press_window { -1 };
    int m_overview_press_tile { -1 };
    bool m_overview_dragging { false };
    bool m_overview_hover_close { false };
    // Moves each display's row of Spaces towards open or closed: true while one's still going.
    bool advance_overview_bar(double now);
    void open_overview_bar(DisplaySpaces&, bool);
    // Where a Space's name is, open or not.
    [[nodiscard]] Gfx::FloatRect overview_label(Display const&, DisplaySpaces const&, int index) const;
    // The menu bar, out of the way as far as this.
    void slide_menu_bars(float amount);

    // Coming into view on a full screen Space, each from 0 to 1.
    float m_reveal { 0 };
    float m_reveal_from { 0 };
    double m_reveal_started { 0 };
    int m_reveal_output { -1 };
    bool m_reveal_wanted { false };
    float m_dock_reveal { 0 };
    float m_dock_reveal_from { 0 };
    double m_dock_reveal_started { 0 };
    bool m_dock_wanted { false };
    bool m_dragging_divider { false };

    // The wallpaper, uncovered.
    bool m_desktop_revealed { false };
    float m_desktop_reveal { 0 };
    float m_desktop_reveal_from { 0 };
    double m_desktop_reveal_started { 0 };
    bool m_click_wallpaper_reveals { true };

    i32 m_hot_corners[4] { 0, 0, 0, 0 };
    int m_hot_corner { -1 };
    int m_hot_corner_timer { -1 };

    // Cmd-Tab, for as long as Cmd is down.
    Vector<Client*> m_recent_applications;
    bool m_switching { false };
    int m_switch_selected { 0 };
    // When the key that brought up the application switcher happened, until the shell's panel for it is on the screen
    // (the budget "Cmd-Tab appears").
    u64 m_switch_asked_at { 0 };

    bool m_stage { false };
    Client* m_stage_shown { nullptr };
    struct StageItem {
        Client* client { nullptr };
        RefPtr<Gfx::Bitmap> picture;
        Gfx::FloatRect rect;
    };
    Vector<StageItem> m_stage_strip;

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
