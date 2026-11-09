/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/InputMethod.h>
#include <AppInstanceEndpoint.h>
#include <AudioEndpoint.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <WorkspaceEndpoint.h>
#include <sandbox.h>
#include <LibJSON/JSON.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Appearance.h>
#include <LibUI/Application.h>
#include <LibUI/Motion.h>
#include <LibUI/WindowRegistry.h>
#include <PasteboardEndpoint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace UI {

using namespace Gfx;

namespace {

Application* s_application;

// When something's measuring how long this program took to get going (VULTURE_REPORT_LAUNCH).
void report_launch_once()
{
    static bool s_reported = false;
    if (s_reported)
        return;
    s_reported = true;
    if (!getenv("VULTURE_REPORT_LAUNCH"))
        return;
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    printf("launch: first window on screen at %lld.%06ld\n", static_cast<long long>(now.tv_sec), static_cast<long>(now.tv_nsec / 1000));
    fflush(stdout);
}

class ServerLayerBackend final : public LayerBackend {
public:
    explicit ServerLayerBackend(NonnullOwnPtr<::Window::Layer> layer)
        : m_layer(move(layer))
    {
    }

    ErrorOr<void> set_surface(IntSize pixels) override { return m_layer->set_surface(pixels); }
    Bitmap& back_buffer() override { return m_layer->back_buffer(); }
    Bitmap const& shown_buffer() const override { return m_layer->shown_buffer(); }

    [[nodiscard]] ::Window::Layer& layer() { return *m_layer; }
    [[nodiscard]] ::Window::Layer const& layer() const { return *m_layer; }

private:
    NonnullOwnPtr<::Window::Layer> m_layer;
};

class ServerBackend final : public WindowBackend {
public:
    ServerBackend(UI::Window& owner, NonnullOwnPtr<::Window::Window> window)
        : m_owner(owner)
        , m_window(move(window))
        , m_transaction(*m_window)
    {
        m_window->on_mouse = [this](::Window::MouseEvent const& event) {
            m_owner.handle_mouse(event.kind, event.position.to<float>(), event.buttons, event.modifiers, event.wheel_x, event.wheel_y);
        };
        m_window->on_key = [this](::Window::KeyEvent const& event) {
            KeyEvent shortcut;
            shortcut.code = event.code;
            shortcut.modifiers = event.modifiers;
            shortcut.text = event.text.view();
            // (With Control alone too, as Show Next Tab's: an item only takes the modifiers it names, so the rest still type.)
            if (event.pressed && (event.modifiers & (::Window::ModifierCommand | ::Window::ModifierControl)) && Application::the().perform_main_menu_shortcut(shortcut))
                return;
            m_owner.handle_key(event.pressed, event.code, event.modifiers, event.text.view());
        };
        m_window->on_resize = [this](IntSize size) {
            m_owner.handle_resized(size.to<float>());
        };
        m_window->on_close = [this] { m_owner.handle_close_request(); };
        m_window->on_activation = [this](bool active) { m_owner.handle_activation(active); };
        m_window->on_presented = [this] {
            report_launch_once();
            m_owner.handle_presented();
        };
        m_window->on_layers_presented = [this](u32) {
            report_launch_once();
            m_owner.handle_presented();
        };
        m_window->on_layer_animation_finished = [this](u32, u32 tag) { m_owner.handle_layer_animation_finished(tag); };
        m_window->on_drag = [this](::Window::DragEvent const& event) {
            m_owner.handle_drag(event.phase, event.position.to<float>(), event.type, event.data);
        };
        m_window->on_drag_finished = [this](bool accepted) { m_owner.handle_drag_finished(accepted); };
    }

    Bitmap& back_buffer() override { return m_window->back_buffer(); }
    void present(IntRect damage) override { m_window->present(damage); }
    int scale() const override { return m_window->connection().scale(); }
    u32 system_id() const override { return m_window->id(); }
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
    void arrange(WS::WindowArrangement arrangement) override { m_window->arrange(arrangement); }
    void toggle_full_screen() override { m_window->toggle_full_screen(); }
    void take_place_of(WindowBackend& other) override { m_window->take_place_of(static_cast<decltype(*this)>(other).m_window->id()); }
    void tile_in_split_view(bool on_left) override { m_window->tile_in_split_view(on_left); }
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

    bool supports_layers() const override { return true; }
    ErrorOr<NonnullOwnPtr<LayerBackend>> create_layer(LayerBackend const* parent) override
    {
        auto layer = TRY(::Window::Layer::create(*m_window, parent ? &static_cast<ServerLayerBackend const*>(parent)->layer() : nullptr));
        return adopt_own(*static_cast<LayerBackend*>(new ServerLayerBackend(move(layer))));
    }
    void layer_set(LayerBackend const& layer, WS::LayerProperty property, float a, float b, float c, float d) override
    {
        m_transaction.set(static_cast<ServerLayerBackend const&>(layer).layer(), property, a, b, c, d);
    }
    void layer_animate(LayerBackend const& layer, WS::LayerProperty property, Span<float const> from, Span<float const> to, Animation::Timing const& timing, u32 tag, bool repeats, int steps) override
    {
        m_transaction.animate(static_cast<ServerLayerBackend const&>(layer).layer(), property, from, to, timing, tag, repeats, steps);
    }
    void layer_destroy(LayerBackend const& layer) override { m_transaction.destroy(static_cast<ServerLayerBackend const&>(layer).layer()); }
    void layer_present(LayerBackend& layer, IntRect damage) override { m_transaction.present(static_cast<ServerLayerBackend&>(layer).layer(), damage); }
    void layer_present_window(IntRect damage) override { m_transaction.present_window(damage); }
    bool has_layer_changes() const override { return !m_transaction.is_empty(); }
    void layer_commit() override { (void)m_transaction.commit(); }

    [[nodiscard]] ::Window::Window& window() { return *m_window; }

private:
    UI::Window& m_owner;
    NonnullOwnPtr<::Window::Window> m_window;
    ::Window::LayerTransaction m_transaction;
};

}

// What finds this application when something would have started it.
class InstanceService final : public AppInstanceStub {
public:
    explicit InstanceService(Application& application)
        : m_application(application)
    {
    }

    void open_arguments(IPC::Sender const& sender, Vector<String> arguments, Vector<String> tokens) override
    {
        if (sender.uid != getuid())
            return;
        for (auto const& token : tokens) {
            if (!token.is_empty())
                (void)sandbox_extension_consume(token.characters());
        }
        Vector<StringView> views;
        for (auto const& argument : arguments)
            views.append(argument.view());
        if (m_application.on_open_arguments)
            m_application.on_open_arguments(views.span());
        reopen(sender);
    }

    void perform_service(IPC::Sender const& sender, String message, String text) override
    {
        if (sender.uid == getuid() && m_application.on_perform_service)
            m_application.on_perform_service(message.view(), text.view());
        reopen(sender);
    }

    void reopen(IPC::Sender const& sender) override
    {
        if (sender.uid != getuid())
            return;
        if (m_application.on_reopen)
            m_application.on_reopen();
        (void)m_application.connection().server().activate_application(static_cast<i32>(getpid()));
    }

private:
    Application& m_application;
};

Application::Application() = default;

ErrorOr<NonnullOwnPtr<Application>> Application::create(StringView name, StringView identifier)
{
    auto application = adopt_own(*new Application);
    application->m_name = TRY(String::from_view(name));
    application->m_connection = TRY(::Window::Connection::connect(application->m_loop));
    s_application = application.ptr();
    Platform::set(application.ptr());
    install_input_context();
    TRY(Appearance::use(application->m_connection->appearance()));
    application->m_connection->on_appearance_change = [](StringView appearance) {
        (void)Appearance::use(appearance);
    };
    auto* raw = application.ptr();
    application->m_connection->on_menu_item_chosen = [raw](u32 item) {
        if (item < raw->m_menu_items.size() && raw->m_menu_items[item]->action)
            raw->m_menu_items[item]->action();
    };
    application->m_connection->on_quit_requested = [raw] { raw->request_quit(); };
    application->m_connection->on_accessibility_requested = [raw](u32 generation) {
        auto answer = Accessibility::describe_windows(WindowRegistry::windows());
        if (!answer.is_error())
            (void)raw->m_connection->server().publish_accessibility(generation, answer.value().view());
    };
    if (!name.is_empty())
        (void)application->m_connection->server().set_application(name, identifier);
    // For golden screenshots: nothing moves or blinks in this program or what it starts.
    if (getenv("VULTURE_STILL")) {
        set_motion_enabled(false);
        set_held_still(true);
    }
    application->m_connection->on_full_screen_change = [raw](u32, bool full) { raw->full_screen_changed(full); };
    application->m_connection->on_motion_change = [](bool reduced) {
        if (!held_still())
            set_motion_enabled(!reduced);
    };
    if (!identifier.is_empty())
        application->register_instance(identifier);
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

void Application::full_screen_changed(bool full)
{
    if (!m_full_screen_item)
        return;
    m_full_screen_item->title = MUST(String::from_view(full ? "Exit Full Screen"_sv : "Enter Full Screen"_sv));
    publish_menus();
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
    return WindowRegistry::main_window();
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
            if (item.heading) {
                MUST(one.set("heading"_sv, JSON::Value(true)));
                MUST(items.append(move(one)));
                continue;
            }
            auto leaf = [this](JSON::Value& into, MenuItem& leaf_item) {
                MUST(into.set("id"_sv, JSON::Value(static_cast<i64>(m_menu_items.size()))));
                MUST(into.set("enabled"_sv, JSON::Value(leaf_item.enabled)));
                MUST(into.set("checked"_sv, JSON::Value(leaf_item.checked)));
                if (!leaf_item.shortcut_text.is_empty())
                    MUST(into.set("shortcut"_sv, MUST(JSON::Value::string(leaf_item.shortcut_text.view()))));
                if (!leaf_item.picture.is_empty()) {
                    auto parts = JSON::Value::array();
                    for (auto const& part : leaf_item.picture) {
                        MUST(parts.append(JSON::Value(static_cast<double>(part.x))));
                        MUST(parts.append(JSON::Value(static_cast<double>(part.y))));
                        MUST(parts.append(JSON::Value(static_cast<double>(part.width))));
                        MUST(parts.append(JSON::Value(static_cast<double>(part.height))));
                    }
                    MUST(into.set("picture"_sv, move(parts)));
                    MUST(into.set("palette"_sv, JSON::Value(leaf_item.palette)));
                }
                m_menu_items.append(&leaf_item);
            };
            if (item.submenu) {
                MUST(one.set("items"_sv, self(self, *item.submenu)));
            } else {
                leaf(one, item);
                if (item.alternate) {
                    auto alternate = JSON::Value::object();
                    MUST(alternate.set("title"_sv, MUST(JSON::Value::string(item.alternate->title.view()))));
                    leaf(alternate, *item.alternate);
                    MUST(one.set("alternate"_sv, move(alternate)));
                }
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

AudioProxy* Application::audio()
{
    if (!m_audio) {
        auto port = IPC::look_up("com.vulture.audioserver"_sv);
        if (port.is_error())
            return nullptr;
        m_audio = adopt_own(*new AudioProxy(port.release_value()));
    }
    return m_audio.ptr();
}

void Application::set_badge(StringView text)
{
    (void)m_connection->server().set_badge(text);
}

void Application::play_sound(StringView name)
{
    auto* server = audio();
    // If it has gone, it starts again: ask for it anew next time.
    if (server && server->play_sound(name).is_error())
        m_audio = nullptr;
}

Optional<u32> Application::output_volume()
{
    auto* server = audio();
    if (!server)
        return {};
    auto answer = server->volume();
    if (answer.is_error()) {
        m_audio = nullptr;
        return {};
    }
    return answer.value();
}

Optional<bool> Application::startup_sound()
{
    auto* server = audio();
    if (!server)
        return {};
    auto answer = server->startup_sound();
    if (answer.is_error()) {
        m_audio = nullptr;
        return {};
    }
    return answer.value();
}

void Application::set_startup_sound(bool plays)
{
    if (auto* server = audio(); server && server->set_startup_sound(plays).is_error())
        m_audio = nullptr;
}

void Application::set_output_volume(u32 percent)
{
    auto* server = audio();
    if (!server)
        return;
    if (server->set_volume(percent).is_error()) {
        m_audio = nullptr;
        return;
    }
    if (m_volume_tick_timer >= 0)
        cancel_timer(m_volume_tick_timer);
    m_volume_tick_timer = add_timer(120, false, [this] {
        m_volume_tick_timer = -1;
        play_sound("volume"_sv);
    });
}

void Application::request_quit()
{
    if (!on_quit_request || on_quit_request())
        quit();
}

void Application::register_instance(StringView identifier)
{
    auto pair = IPC::create_port();
    if (pair.is_error())
        return;
    auto ports = pair.release_value();
    StringBuilder name;
    name.append("com.vulture.app."_sv);
    name.append(identifier);
    // Someone started a second one by its program, which is allowed: it's the first that goes on being found.
    if (IPC::register_service(name.view(), move(ports.send)).is_error())
        return;
    m_instance = adopt_own(*new InstanceService(*this));
    m_instance_listener = adopt_own(*new IPC::Listener(m_loop, move(ports.receive), [this](IPC::ReceivedMessage& message) { (void)m_instance->handle(message); }));
}

String Application::home_directory() const
{
    char const* home = getenv("HOME");
    return MUST(String::from_view(home ? StringView(home, strlen(home)) : StringView {}));
}

ErrorOr<void> Application::make_directory(StringView path)
{
    if (mkdir(TRY(String::from_view(path)).characters(), 0755) != 0)
        return Error::from_errno(errno);
    return {};
}

// What the Services menu offers: what applications say they can do with text (NSServices, in their Info.plist).
void Application::add_services_to(Menu& menu)
{
    auto scan = [&](StringView directory) {
        auto listed = list_directory(directory);
        if (listed.is_error())
            return;
        for (auto const& entry : listed.value()) {
            if (!entry.name.view().ends_with(".app"_sv))
                continue;
            StringBuilder bundle;
            bundle.appendff("{}/{}", directory, entry.name.view());
            StringBuilder plist;
            plist.appendff("{}/Contents/Info.plist", bundle.view());
            auto bytes = read_file(plist.view());
            if (bytes.is_error())
                continue;
            auto parsed = JSON::parse({ reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size() });
            if (parsed.is_error())
                continue;
            auto const* services = parsed.value().get_array("NSServices"_sv);
            if (!services)
                continue;
            for (size_t i = 0; i < services->size(); ++i) {
                auto title = services->at(i).get_string("NSMenuItem"_sv);
                auto message = services->at(i).get_string("NSMessage"_sv);
                if (!title.has_value() || !message.has_value())
                    continue;
                auto bundle_path = MUST(bundle.to_string());
                auto message_copy = MUST(String::from_view(message.value()));
                menu.add(title.value(), [this, bundle_path = move(bundle_path), message_copy = move(message_copy)] { perform_service(bundle_path.view(), message_copy.view()); });
            }
        }
    };
    scan("/Applications"_sv);
    scan("/System/Applications"_sv);
    if (menu.items().is_empty())
        menu.add("No Services Apply"_sv).enabled = false;
}

void Application::perform_service(StringView bundle, StringView message)
{
    auto* window = key_window();
    auto* responder = window ? window->first_responder() : nullptr;
    auto* view = responder ? responder->as_view() : nullptr;
    auto selection = view ? view->selected_text_for_services() : StringView {};
    if (selection.is_empty()) {
        play_sound("error"_sv);
        return;
    }
    auto port = IPC::look_up("com.vulture.workspace"_sv);
    if (port.is_error())
        return;
    WorkspaceProxy workspace(port.release_value());
    (void)workspace.perform_service(bundle, message, selection);
}

NonnullOwnPtr<Menu> Application::standard_menus(Function<void()> on_about, bool can_quit)
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
    add_services_to(application_menu.add_submenu("Services"_sv));
    application_menu.add_separator();
    application_menu.add_with_shortcut(hide_title.view(), KeyH, shown("H"_sv).view(), [this] {
        (void)m_connection->server().hide_application(static_cast<i32>(getpid()), true);
    });
    StringBuilder hide_others;
    hide_others.append("\xE2\x8C\xA5"_sv);
    hide_others.append(cmd);
    hide_others.append("H"_sv);
    application_menu.add_with_shortcut("Hide Others"_sv, KeyH, hide_others.view(), [this] { (void)m_connection->server().hide_others(true); }, WS::ModifierCommand | WS::ModifierOption);
    application_menu.add("Show All"_sv, [this] { (void)m_connection->server().hide_others(false); });
    if (can_quit) {
        application_menu.add_separator();
        application_menu.add_with_shortcut(quit_title.view(), KeyQ, shown("Q"_sv).view(), [this] { request_quit(); });
    }

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

    edit.add_separator();
    edit.add("Emoji & Symbols"_sv, [this] { (void)send_command("emojiAndSymbols"_sv); });

    auto& window_menu = menus->add_submenu("Window"_sv);
    window_menu.add_with_shortcut("Minimize"_sv, KeyM, shown("M"_sv).view(), [this] {
        if (auto* window = key_window())
            window->backend().minimize();
    });
    window_menu.add("Zoom"_sv, [this] {
        if (auto* window = key_window())
            window->backend().zoom();
    });
    auto arranging = [this](WS::WindowArrangement arrangement) {
        return [this, arrangement] {
            if (auto* window = key_window())
                window->backend().arrange(arrangement);
        };
    };
    // Sequoia's order: the active window's place, then its tiles, then full screen, then tabs.
    window_menu.add("Fill"_sv, arranging(WS::WindowArrangement::Fill));
    window_menu.add("Center"_sv, arranging(WS::WindowArrangement::Center));
    window_menu.add_separator();
    auto& move_and_resize = window_menu.add_submenu("Move & Resize"_sv);
    auto pictured = [&](Menu& menu, StringView title, WS::WindowArrangement arrangement) -> MenuItem& {
        auto& item = menu.add(title, arranging(arrangement));
        item.picture = picture_of_arrangement(arrangement);
        return item;
    };
    auto alternate = [&](MenuItem& item, StringView title, WS::WindowArrangement arrangement) {
        Menu::add_alternate(item, title, arranging(arrangement)).picture = picture_of_arrangement(arrangement);
    };
    (void)pictured(move_and_resize, "Left"_sv, WS::WindowArrangement::Left);
    (void)pictured(move_and_resize, "Right"_sv, WS::WindowArrangement::Right);
    (void)pictured(move_and_resize, "Top"_sv, WS::WindowArrangement::Top);
    (void)pictured(move_and_resize, "Bottom"_sv, WS::WindowArrangement::Bottom);
    move_and_resize.add_separator();
    (void)pictured(move_and_resize, "Top Left"_sv, WS::WindowArrangement::TopLeft);
    (void)pictured(move_and_resize, "Top Right"_sv, WS::WindowArrangement::TopRight);
    (void)pictured(move_and_resize, "Bottom Left"_sv, WS::WindowArrangement::BottomLeft);
    (void)pictured(move_and_resize, "Bottom Right"_sv, WS::WindowArrangement::BottomRight);
    move_and_resize.add_separator();
    // The windows under it too; with Option, three of them.
    alternate(pictured(move_and_resize, "Left & Right"_sv, WS::WindowArrangement::LeftAndRight), "Left & Quarters"_sv, WS::WindowArrangement::LeftAndQuarters);
    alternate(pictured(move_and_resize, "Right & Left"_sv, WS::WindowArrangement::RightAndLeft), "Right & Quarters"_sv, WS::WindowArrangement::RightAndQuarters);
    alternate(pictured(move_and_resize, "Top & Bottom"_sv, WS::WindowArrangement::TopAndBottom), "Top & Quarters"_sv, WS::WindowArrangement::TopAndQuarters);
    alternate(pictured(move_and_resize, "Bottom & Top"_sv, WS::WindowArrangement::BottomAndTop), "Bottom & Quarters"_sv, WS::WindowArrangement::BottomAndQuarters);
    (void)pictured(move_and_resize, "Quarters"_sv, WS::WindowArrangement::Quarters);
    move_and_resize.add_separator();
    move_and_resize.add("Return to Previous Size"_sv, arranging(WS::WindowArrangement::Previous));
    auto& full_screen_tile = window_menu.add_submenu("Full Screen Tile"_sv);
    full_screen_tile.add("Left of Screen"_sv, [this] {
        if (auto* window = key_window())
            window->backend().tile_in_split_view(true);
    }).picture = picture_of_split_view(true);
    full_screen_tile.add("Right of Screen"_sv, [this] {
        if (auto* window = key_window())
            window->backend().tile_in_split_view(false);
    }).picture = picture_of_split_view(false);
    {
        StringBuilder full;
        full.append("\xE2\x8C\x83"_sv);
        full.append(cmd);
        full.append("F"_sv);
        // (Said by the server, which sees it first, and works when it's the title bar that's hidden too.)
        m_full_screen_item = &window_menu.add_with_shortcut("Enter Full Screen"_sv, KeyF, full.view(), [this] {
            if (auto* window = key_window())
                window->backend().toggle_full_screen();
        }, WS::ModifierCommand | WS::ModifierControl);
    }
    window_menu.add_separator();
    {
        StringBuilder previous;
        previous.append("\xE2\x8C\x83\xE2\x87\xA7\xE2\x87\xA5"_sv);
        window_menu.add_with_shortcut("Show Previous Tab"_sv, KeyTab, previous.view(), [this] {
            if (auto* window = key_window())
                window->show_adjacent_tab(-1);
        }, WS::ModifierControl | WS::ModifierShift);
        StringBuilder next;
        next.append("\xE2\x8C\x83\xE2\x87\xA5"_sv);
        window_menu.add_with_shortcut("Show Next Tab"_sv, KeyTab, next.view(), [this] {
            if (auto* window = key_window())
                window->show_adjacent_tab(1);
        }, WS::ModifierControl);
    }
    window_menu.add("Move Tab to New Window"_sv, [this] {
        if (auto* window = key_window())
            window->move_tab_to_new_window();
    });
    window_menu.add("Merge All Windows"_sv, [this] {
        if (auto* window = key_window())
            window->merge_all_windows();
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
    raw->on_discard = [this, raw] {
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
        // What Files hides.
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
