/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibBase/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibUI/Menu.h>
#include <LibUI/Platform.h>
#include <LibWindow/Window.h>

// A program with windows: the event loop, the link to the window server, and what
// belongs to no one window. There is one.
//
//     auto app = MUST(UI::Application::create());
//     auto& window = app->make_window<UI::Window>("Hello"_sv, Gfx::IntRect { 100, 100, 400, 300 });
//     window.show();
//     return app->exec();

class AudioProxy;

namespace UI {

class Application final : public Platform {
public:
    // With a name, an application, shown in the Dock and menu bar; without, a part of the
    // desktop itself.
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Application>> create(StringView name = {}, StringView identifier = {});
    ~Application() override;

    [[nodiscard]] static Application& the();

    [[nodiscard]] int exec() { return m_loop.exec(); }
    void quit(int code = 0) { m_loop.quit(code); }
    // Asks to quit, from the menu or the Dock: on_quit_request may say no, and see to
    // what it objected to (a document not saved) before quitting itself.
    void request_quit();
    Function<bool()> on_quit_request;
    [[nodiscard]] Core::EventLoop& event_loop() { return m_loop; }
    [[nodiscard]] ::Window::Connection& connection() { return *m_connection; }
    [[nodiscard]] StringView name() const { return m_name.view(); }

    // The menus of the program, for the menu bar; and their shortcuts, in any window.
    void set_main_menu(NonnullOwnPtr<Menu>);
    [[nodiscard]] Menu* main_menu() const { return m_main_menu.ptr(); }
    bool perform_main_menu_shortcut(KeyEvent const&);
    // The usual menus, for set_main_menu: About, Hide and Quit under the program's name;
    // and Edit and Window, which act on what has the keyboard. `extra` go between
    // the first and Edit.
    [[nodiscard]] NonnullOwnPtr<Menu> standard_menus(Function<void()> on_about = nullptr, bool can_quit = true);
    // Says the menus again, after a change to them.
    void publish_menus();
    // What has the keyboard: gets "copy" and the like, up the responder chain.
    bool send_command(StringView command);
    [[nodiscard]] Window* key_window() const;

    // Windows the application keeps until they close.
    template<typename T, typename... Args>
    T& make_window(Args&&... arguments)
    {
        auto* window = new T(forward<Args>(arguments)...);
        keep_window(adopt_own(*static_cast<Window*>(window)));
        return *window;
    }
    void keep_window(NonnullOwnPtr<Window>);
    // Quits when the last one kept goes.
    void set_quits_with_last_window(bool quits) { m_quits_with_last_window = quits; }

    // What everything plays at, 0 to 100, when there is a sound server to ask. Setting it
    // plays a tick once the change has settled, to hear the new level by.
    [[nodiscard]] Optional<u32> output_volume();
    void set_output_volume(u32 percent);

    // Platform.
    [[nodiscard]] ErrorOr<NonnullOwnPtr<WindowBackend>> create_window(Window& owner, StringView title, Gfx::IntRect frame, u32 flags) override;
    [[nodiscard]] Gfx::IntSize screen_size() const override { return m_connection->screen_size(); }
    [[nodiscard]] int scale() const override { return m_connection->scale(); }
    int add_timer(u64 milliseconds, bool repeating, Function<void()> callback) override { return m_loop.add_timer(milliseconds, repeating, move(callback)); }
    void cancel_timer(int id) override { m_loop.cancel_timer(id); }
    void defer(Function<void()> callback) override { m_loop.deferred_invoke(move(callback)); }
    [[nodiscard]] double now() const override;
    [[nodiscard]] ErrorOr<Vector<u8>> read_file(StringView path) override;
    [[nodiscard]] StringView library_directory() const override { return "/System/Library"_sv; }
    [[nodiscard]] ErrorOr<Vector<DirectoryEntry>> list_directory(StringView path) override;
    void copy_text(StringView) override;
    [[nodiscard]] ErrorOr<String> paste_text() override;
    void play_sound(StringView) override;

private:
    Application();

    [[nodiscard]] AudioProxy* audio();

    Core::EventLoop m_loop;
    OwnPtr<::Window::Connection> m_connection;
    OwnPtr<AudioProxy> m_audio;
    int m_volume_tick_timer { -1 };
    OwnPtr<Menu> m_main_menu;
    Vector<MenuItem*> m_menu_items;
    Vector<NonnullOwnPtr<Window>> m_windows;
    String m_name;
    bool m_quits_with_last_window { true };
};

}
