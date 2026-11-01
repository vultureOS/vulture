/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <LibUI/Appearance.h>
#include <LibUI/Application.h>
#include <PasteboardEndpoint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
    void begin_drag(WindowBackend& image, StringView type, ReadonlyBytes data) override
    {
        m_window->begin_drag(static_cast<ServerBackend&>(image).window(), type, data);
    }
    void accept_drop(bool accepted) override { m_window->accept_drop(accepted); }
    void set_materials(Span<MaterialRegion const> regions) override
    {
        Vector<::Window::Window::MaterialRegion> converted;
        for (auto const& region : regions)
            converted.append(::Window::Window::MaterialRegion { region.rect, region.material });
        m_window->set_materials(converted.span());
    }

    [[nodiscard]] ::Window::Window& window() { return *m_window; }

private:
    UI::Window& m_owner;
    NonnullOwnPtr<::Window::Window> m_window;
};

}

Application::Application() = default;

ErrorOr<NonnullOwnPtr<Application>> Application::create(StringView name)
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
