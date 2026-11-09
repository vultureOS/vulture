/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Span.h>
#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibJSON/JSON.h>
#include <LibUI/Menu.h>
#include <LibUI/Window.h>
#include <sys/types.h>

namespace Core {
class EventLoop;
}

namespace IPC {
class Listener;
}

namespace Desktop {

// An application bundle: Name.app/Contents/Info.plist (JSON, as all plists here) and
// the program in Contents/MacOS.
struct AppInfo {
    String name;
    String identifier;
    String bundle_path;
    String executable_path;
    String icon;
    // What it is, in plain words ("Terminal", for Terminal): shown beside its name, and
    // what searching for that finds it by.
    String kind;
    // What kind of application, as a Mac's LSApplicationCategoryType says: public.app-category.utilities.
    String category;
    // What it runs in: a profile in /System/Library/Sandbox/Profiles. Empty: it isn't.
    String sandbox;
    // File extensions it opens.
    Vector<String> document_extensions;
};

// In /Applications and /System/Applications, by name.
[[nodiscard]] Vector<AppInfo> installed_applications();
[[nodiscard]] ErrorOr<AppInfo> read_bundle(StringView bundle_path);
[[nodiscard]] Optional<AppInfo> application_with_identifier(StringView identifier);

// Starts it, with these arguments, as a new session of its own. EINVAL for no program.
ErrorOr<pid_t> launch(AppInfo const&, Span<StringView const> arguments);
ErrorOr<pid_t> launch(AppInfo const&);
// Given to it instead, if it's already running: this says whether it was, and then nothing started.
[[nodiscard]] bool tell_running_application(AppInfo const&, Span<StringView const> arguments);
// `message`, to do to `text`, in that application, started if need be: what a choice in the Services menu comes to.
void perform_service(AppInfo const&, StringView message, StringView text);
// As launch, but not as a child of this process: by way of the desktop, for what is sandboxed.
ErrorOr<pid_t> open_application(AppInfo const&);
// Opens a file or a folder with what handles it, or an application bundle.
ErrorOr<void> open_path(StringView path);
// With this application, whatever would open it otherwise.
ErrorOr<void> open_path_with(StringView path, StringView application_bundle);
// In Files: the folder it's in.
ErrorOr<void> reveal_in_files(StringView path);

// An icon of /System/Library/Icons, at a size, for this display. Cached.
[[nodiscard]] RefPtr<Gfx::Bitmap> icon_named(StringView name, int points, int scale);
// What a file shows as: a folder's, an application's own, a document's.
[[nodiscard]] String icon_name_for_path(StringView path);

// What the window server says of the session.
struct WindowEntry {
    u32 id { 0 };
    String title;
    bool minimized { false };
    int space { 0 };
};

struct ApplicationEntry {
    i32 pid { 0 };
    String name;
    String identifier;
    bool hidden { false };
    bool not_responding { false };
    String badge;
    Vector<WindowEntry> windows;
};

struct ShellState {
    i32 active_pid { 0 };
    String active_name;
    String active_identifier;
    String menus;
    int space { 0 };
    int space_count { 1 };
    bool overview { false };
    Vector<ApplicationEntry> applications;
    // Cmd-Tab, while it's going on: by pid, the most recent first.
    Vector<i32> switcher;
    int switcher_selected { 0 };
    bool stage { false };
    bool desktop_revealed { false };
};
[[nodiscard]] ErrorOr<ShellState> parse_shell_state(StringView json);

// A menu described by an application's published JSON, whose items call `chosen`.
[[nodiscard]] NonnullOwnPtr<UI::Menu> menu_from_json(JSON::Value const& description, Function<void(u32)> const& chosen);

// A program's preferences: ~/Library/Preferences/<identifier>.json.
class Preferences {
public:
    explicit Preferences(StringView identifier);

    [[nodiscard]] Optional<StringView> string(StringView key) const { return m_values.get_string(key); }
    [[nodiscard]] Optional<double> number(StringView key) const { return m_values.get_number(key); }
    [[nodiscard]] Optional<bool> boolean(StringView key) const { return m_values.get_boolean(key); }
    [[nodiscard]] JSON::Value const* array(StringView key) const { return m_values.get_array(key); }
    [[nodiscard]] JSON::Value const* object(StringView key) const { return m_values.get_object(key); }
    void set(StringView key, JSON::Value);
    void save();

private:
    String m_path;
    JSON::Value m_values;
};

// A menu extra: a program's own place in the menu bar, as a Mac's are, put there by the window server to the left of the
// shell's. It draws its symbol, and says when it's pressed, with where it is on the screen, for what it opens to go under.
class MenuExtra {
public:
    MenuExtra(StringView name, float width, Function<void(UI::Graphics&, Gfx::FloatRect, Gfx::Color)> draw);
    ~MenuExtra();

    void show();
    void hide();
    // Lit, as a menu's title is, while what it opened is open.
    void set_highlighted(bool);
    [[nodiscard]] Gfx::IntRect screen_rect() const;
    Function<void(Gfx::IntRect)> on_press;

private:
    class ExtraView;
    OwnPtr<UI::Window> m_window;
    ExtraView* m_view { nullptr };
};

// The home of whoever this is.
[[nodiscard]] String home_directory();

// Posting a notification, as an application does: it's shown as that application's settings in Settings > Notifications
// say. What it's known by comes back, for what's done with it: 0 when it wasn't let through. Its actions are what can be
// done about it, the first being the one shown when there's room for only one.
u32 post_notification(StringView application, StringView title, StringView body, Span<StringView const> actions = {});

// What's done with an application's notifications: one of its actions chosen, or (with an empty action) the notification
// itself clicked. The newest of an application's to listen is the one that hears.
class NotificationResponses {
public:
    NotificationResponses(Core::EventLoop&, StringView application);
    ~NotificationResponses();

    Function<void(u32 id, StringView action)> on_response;

private:
    class Stub;
    OwnPtr<Stub> m_stub;
    OwnPtr<IPC::Listener> m_listener;
};

// The system's mark, in a square.
void draw_logo(UI::Graphics&, Gfx::FloatRect const&, Gfx::Color);

// Asks whoever keeps the session to end it.
void log_out();
void restart();
void shut_down();
void lock_screen();
void start_screen_saver();
// Fast user switching: the login window, with this person chosen (or the list, for nobody in particular).
void switch_to_user(StringView name);
// Who's logged in, by short name.
[[nodiscard]] Vector<String> logged_in_users();

// A person's picture: their initials, white on grey, as a Mac makes one for whoever hasn't chosen a picture.
void draw_monogram(UI::Graphics&, Gfx::FloatRect, StringView full_name);

// Login Items: what opens when this person logs in, by path, as Settings > Login Items and the Dock's Options say
// (~/Library/Preferences/com.vulture.loginitems.json).
[[nodiscard]] Vector<String> login_items();
[[nodiscard]] bool is_login_item(StringView path);
void set_login_item(StringView path, bool opens);

}
