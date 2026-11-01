/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <LibJSON/JSON.h>
#include <LibUI/Appearance.h>
#include <LibUI/Application.h>
#include <LibUI/Motion.h>
#include <LibUI/WindowRegistry.h>
#include <PasteboardEndpoint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace UI {

using namespace Gfx;

namespace {

Application* s_application;

class ServerBackend final : public WindowBackend {
public:
    ServerBackend(UI::Window& owner, NonnullOwnPtr<::Window::Window> window)
        : m_owner(owner)
        , m_window(move(window))
    {
        m_window->on_mouse = [this](::Window::MouseEvent const& event) {
            m_owner.handle_mouse(event.kind, event.position.to<float>(), event.buttons, event.modifiers, event.wheel_x, event.wheel_y);
        };
        m_window->on_key = [this](::Window::KeyEvent const& event) {
            KeyEvent shortcut;
            shortcut.code = event.code;
            shortcut.modifiers = event.modifiers;
            shortcut.text = event.text.view();
            if (event.pressed && (event.modifiers & ::Window::ModifierCommand) && Application::the().perform_main_menu_shortcut(shortcut))
                return;
            m_owner.handle_key(event.pressed, event.code, event.modifiers, event.text.view());
        };
        m_window->on_resize = [this](IntSize size) {
            m_owner.handle_resized(size.to<float>());
        };
        m_window->on_close = [this] { m_owner.handle_close_request(); };
        m_window->on_activation = [this](bool active) { m_owner.handle_activation(active); };
        m_window->on_presented = [this] { m_owner.handle_presented(); };
        m_window->on_drag = [this](::Window::DragEvent const& event) {
            m_owner.handle_drag(event.phase, event.position.to<float>(), event.type, event.data);
        };
        m_window->on_drag_finished = [this](bool accepted) { m_owner.handle_drag_finished(accepted); };
    }

    Bitmap& back_buffer() override { return m_window->back_buffer(); }
    void present(IntRect damage) override { m_window->present(damage); }
    int scale() const override { return m_window->connection().scale(); }
    IntRect frame() const override { return m_window->frame(); }
    void set_frame(IntRect frame) override { m_window->set_frame(frame); }
    void set_title(StringView title) override { m_window->set_title(title); }
    void show(bool visible) override { m_window->show(visible); }
    void activate() override { m_window->activate(); }
    void set_level(int level) override { m_window->set_level(level); }
    void set_corner_radius(int radius) override { m_window->set_corner_radius(radius); }
    void set_title_bar_height(int height) override { m_window->set_title_bar_height(height); }
    void begin_move() override { m_window->begin_move(); }
    void grab_pointer(bool grab) override { m_window->grab_pointer(grab); }
    void set_cursor(WS::CursorShape shape) override { m_window->set_cursor(shape); }
    void minimize() override { m_window->minimize(); }
    void zoom() override { m_window->zoom(); }
    void set_input_region(IntRect region) override { m_window->set_input_region(region); }
    void begin_drag(WindowBackend& image, StringView type, ReadonlyBytes data) override
    {
        m_window->begin_drag(static_cast<ServerBackend&>(image).window(), type, data);
    }
    void accept_drop(bool accepted) override { m_window->accept_drop(accepted); }
    void set_materials(Span<MaterialRegion const> regions) override
    {
        Vector<::Window::Window::MaterialRegion> converted;
        for (auto const& region : regions)
            converted.append(::Window::Window::MaterialRegion { region.rect, region.material, region.radius });
        m_window->set_materials(converted.span());
    }

    [[nodiscard]] ::Window::Window& window() { return *m_window; }

private:
    UI::Window& m_owner;
    NonnullOwnPtr<::Window::Window> m_window;
};

}

Application::Application() = default;

ErrorOr<NonnullOwnPtr<Application>> Application::create(StringView name, StringView identifier)
{
    auto application = adopt_own(*new Application);
    application->m_name = TRY(String::from_view(name));
    application->m_connection = TRY(::Window::Connection::connect(application->m_loop));
    s_application = application.ptr();
    Platform::set(application.ptr());
    TRY(Appearance::use(application->m_connection->appearance()));
    application->m_connection->on_appearance_change = [](StringView appearance) {
        (void)Appearance::use(appearance);
    };
    auto* raw = application.ptr();
    application->m_connection->on_menu_item_chosen = [raw](u32 item) {
        if (item < raw->m_menu_items.size() && raw->m_menu_items[item]->action)
            raw->m_menu_items[item]->action();
    };
    application->m_connection->on_quit_requested = [raw] { raw->quit(); };
    if (!name.is_empty())
        (void)application->m_connection->server().set_application(name, identifier);
    // For golden screenshots: nothing moves or blinks in this program or what it starts.
    if (getenv("VULTURE_STILL"))
        set_motion_enabled(false);
    return application;
}

Application::~Application()
{
    m_windows.clear();
    s_application = nullptr;
    Platform::set(nullptr);
}

Application& Application::the()
{
    VERIFY(s_application);
    return *s_application;
}

void Application::set_main_menu(NonnullOwnPtr<Menu> menu)
{
    m_main_menu = move(menu);
    publish_menus();
}

Window* Application::key_window() const
{
    for (auto* window : WindowRegistry::windows()) {
        if (window->is_key())
            return window;
    }
    return nullptr;
}

bool Application::send_command(StringView command)
{
    auto* window = key_window();
    if (!window)
        return false;
    for (Responder* responder = window->first_responder(); responder; responder = responder->next_responder()) {
        if (responder->perform_command(command))
            return true;
    }
    return false;
}

void Application::publish_menus()
{
    if (!m_main_menu)
        return;
    m_menu_items.clear();
    // Each item's number is its place in m_menu_items.
    auto describe = [this](auto& self, Menu& menu) -> JSON::Value {
        auto items = JSON::Value::array();
        for (auto& item : menu.items()) {
            auto one = JSON::Value::object();
            if (item.separator) {
                MUST(one.set("separator"_sv, JSON::Value(true)));
                MUST(items.append(move(one)));
                continue;
            }
            MUST(one.set("title"_sv, MUST(JSON::Value::string(item.title.view()))));
            if (item.submenu) {
                MUST(one.set("items"_sv, self(self, *item.submenu)));
            } else {
                MUST(one.set("id"_sv, JSON::Value(static_cast<i64>(m_menu_items.size()))));
                MUST(one.set("enabled"_sv, JSON::Value(item.enabled)));
                MUST(one.set("checked"_sv, JSON::Value(item.checked)));
                if (!item.shortcut_text.is_empty())
                    MUST(one.set("shortcut"_sv, MUST(JSON::Value::string(item.shortcut_text.view()))));
                m_menu_items.append(&item);
            }
            MUST(items.append(move(one)));
        }
        return items;
    };
    auto top = JSON::Value::array();
    for (auto& item : m_main_menu->items()) {
        if (!item.submenu)
            continue;
        auto one = JSON::Value::object();
        MUST(one.set("title"_sv, MUST(JSON::Value::string(item.title.view()))));
        MUST(one.set("items"_sv, describe(describe, *item.submenu)));
        MUST(top.append(move(one)));
    }
    auto text = MUST(top.serialize());
    (void)m_connection->server().set_menus(text.view());
}

NonnullOwnPtr<Menu> Application::standard_menus(Function<void()> on_about)
{
    auto menus = adopt_own(*new Menu);
    StringView cmd = "\xE2\x8C\x98"_sv;
    auto shown = [&](StringView key) {
        StringBuilder builder;
        builder.append(cmd);
        builder.append(key);
        return MUST(builder.to_string());
    };
    auto about_title = [&] {
        StringBuilder builder;
        builder.append("About "_sv);
        builder.append(m_name.view());
        return MUST(builder.to_string());
    }();
    auto hide_title = [&] {
        StringBuilder builder;
        builder.append("Hide "_sv);
        builder.append(m_name.view());
        return MUST(builder.to_string());
    }();
    auto quit_title = [&] {
        StringBuilder builder;
        builder.append("Quit "_sv);
        builder.append(m_name.view());
        return MUST(builder.to_string());
    }();
    auto& application_menu = menus->add_submenu(m_name.view());
    application_menu.add(about_title.view(), move(on_about));
    application_menu.add_separator();
    application_menu.add_with_shortcut(hide_title.view(), KeyH, shown("H"_sv).view(), [this] {
        (void)m_connection->server().hide_application(static_cast<i32>(getpid()), true);
    });
    application_menu.add_separator();
    application_menu.add_with_shortcut(quit_title.view(), KeyQ, shown("Q"_sv).view(), [this] { quit(); });

    auto& edit = menus->add_submenu("Edit"_sv);
    edit.add_with_shortcut("Undo"_sv, KeyZ, shown("Z"_sv).view(), [this] { (void)send_command("undo"_sv); });
    StringBuilder redo;
    redo.append("\xE2\x87\xA7"_sv);
    redo.append(cmd);
    redo.append("Z"_sv);
    edit.add_with_shortcut("Redo"_sv, KeyZ, redo.view(), [this] { (void)send_command("redo"_sv); }, WS::ModifierCommand | WS::ModifierShift);
    edit.add_separator();
    edit.add_with_shortcut("Cut"_sv, KeyX, shown("X"_sv).view(), [this] { (void)send_command("cut"_sv); });
    edit.add_with_shortcut("Copy"_sv, KeyC, shown("C"_sv).view(), [this] { (void)send_command("copy"_sv); });
    edit.add_with_shortcut("Paste"_sv, KeyV, shown("V"_sv).view(), [this] { (void)send_command("paste"_sv); });
    edit.add_with_shortcut("Select All"_sv, KeyA, shown("A"_sv).view(), [this] { (void)send_command("selectAll"_sv); });

    auto& window_menu = menus->add_submenu("Window"_sv);
    window_menu.add_with_shortcut("Minimize"_sv, KeyM, shown("M"_sv).view(), [this] {
        if (auto* window = key_window())
            window->backend().minimize();
    });
    window_menu.add("Zoom"_sv, [this] {
        if (auto* window = key_window())
            window->backend().zoom();
    });
    return menus;
}

bool Application::perform_main_menu_shortcut(KeyEvent const& event)
{
    return m_main_menu && m_main_menu->perform_shortcut(event);
}

void Application::keep_window(NonnullOwnPtr<Window> window)
{
    auto* raw = window.ptr();
    auto previous = move(raw->on_closed);
    raw->on_closed = [this, raw, previous = move(previous)] {
        if (previous)
            previous();
        // Not now: it is telling us this from inside itself.
        m_loop.deferred_invoke([this, raw] {
            for (size_t i = 0; i < m_windows.size(); ++i) {
                if (m_windows[i].ptr() == raw) {
                    m_windows.remove(i);
                    break;
                }
            }
            if (m_windows.is_empty() && m_quits_with_last_window)
                m_loop.quit();
        });
    };
    m_windows.append(move(window));
}

ErrorOr<NonnullOwnPtr<WindowBackend>> Application::create_window(Window& owner, StringView title, IntRect frame, u32 flags)
{
    auto window = TRY(::Window::Window::create(*m_connection, title, frame, flags));
    return adopt_own(*static_cast<WindowBackend*>(new ServerBackend(owner, move(window))));
}

double Application::now() const
{
    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1'000'000'000.0;
}

ErrorOr<Vector<u8>> Application::read_file(StringView path)
{
    auto terminated = TRY(String::from_view(path));
    int fd = open(terminated.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    u8 chunk[65536];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) {
        size_t at = bytes.size();
        TRY(bytes.try_resize(at + static_cast<size_t>(got)));
        memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return bytes;
}

ErrorOr<Vector<Platform::DirectoryEntry>> Application::list_directory(StringView path)
{
    auto terminated = TRY(String::from_view(path));
    DIR* directory = opendir(terminated.characters());
    if (!directory)
        return Error::from_errno(errno);
    Vector<DirectoryEntry> entries;
    while (auto* entry = readdir(directory)) {
        StringView name { entry->d_name, strlen(entry->d_name) };
        // What Finder hides.
        if (name.starts_with('.'))
            continue;
        StringBuilder full;
        full.append(path);
        full.append("/"_sv);
        full.append(name);
        auto full_path = TRY(full.to_string());
        struct stat info;
        bool is_directory = stat(full_path.characters(), &info) == 0 && S_ISDIR(info.st_mode);
        TRY(entries.try_append(DirectoryEntry { TRY(String::from_view(name)), is_directory }));
    }
    closedir(directory);
    return entries;
}

void Application::copy_text(StringView text)
{
    auto port = IPC::look_up("com.vulture.pasteboard"_sv);
    if (port.is_error())
        return;
    PasteboardProxy pasteboard(port.release_value());
    (void)pasteboard.set("general"_sv, "public.utf8-plain-text"_sv, ReadonlyBytes(reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length()));
}

ErrorOr<String> Application::paste_text()
{
    PasteboardProxy pasteboard(TRY(IPC::look_up("com.vulture.pasteboard"_sv)));
    auto answer = TRY(pasteboard.get("general"_sv, "public.utf8-plain-text"_sv));
    if (!answer.found)
        return String {};
    return String::from_view(StringView(reinterpret_cast<char const*>(answer.data.data()), answer.data.size()));
}

}
