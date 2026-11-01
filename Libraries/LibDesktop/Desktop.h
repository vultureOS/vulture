/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibJSON/JSON.h>
#include <LibUI/Menu.h>
#include <sys/types.h>

namespace Desktop {

// An application bundle: Name.app/Contents/Info.plist (JSON, as all plists here) and
// the program in Contents/MacOS.
struct AppInfo {
    String name;
    String identifier;
    String bundle_path;
    String executable_path;
    String icon;
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
// Opens a file or a folder with what handles it, or an application bundle.
ErrorOr<void> open_path(StringView path);

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
    Vector<WindowEntry> windows;
};

struct ShellState {
    i32 active_pid { 0 };
    String active_name;
    String active_identifier;
    String menus;
    int space { 0 };
    int space_count { 1 };
    bool mission_control { false };
    Vector<ApplicationEntry> applications;
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
    void set(StringView key, JSON::Value);
    void save();

private:
    String m_path;
    JSON::Value m_values;
};

// The home of whoever this is.
[[nodiscard]] String home_directory();

void post_notification(StringView application, StringView title, StringView body);

// The system's mark, in a square.
void draw_logo(UI::Graphics&, Gfx::FloatRect const&, Gfx::Color);

// Asks whoever keeps the session to end it.
void log_out();
void restart();
void shut_down();

}
