/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <WorkspaceEndpoint.h>
#include <os/log.h>
#include "Desktop.h"
#include "ContentTypes.h"
#include "Documents.h"
#include <AppInstanceEndpoint.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibGfx/Gradient.h>
#include <LibGfx/Path.h>
#include <LibImage/Image.h>
#include <LibUI/Application.h>
#include <FilesEndpoint.h>
#include <NotificationsEndpoint.h>
#include <SessionEndpoint.h>
#include <dirent.h>
#include <sandbox.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Desktop {

using namespace Gfx;

namespace {

ErrorOr<Vector<u8>> read_whole_file(StringView path)
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

String joined(StringView a, StringView b)
{
    StringBuilder builder;
    builder.append(a);
    if (!a.ends_with('/'))
        builder.append("/"_sv);
    builder.append(b);
    return MUST(builder.to_string());
}

String copy(StringView text)
{
    return MUST(String::from_view(text));
}

bool is_directory(StringView path)
{
    auto terminated = MUST(String::from_view(path));
    struct stat info;
    return stat(terminated.characters(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool comes_after(StringView a, StringView b)
{
    size_t common = min(a.length(), b.length());
    for (size_t i = 0; i < common; ++i) {
        if (a[i] != b[i])
            return a[i] > b[i];
    }
    return a.length() > b.length();
}

struct CachedIcon {
    String name;
    int points;
    int scale;
    RefPtr<Bitmap> bitmap;
};
Vector<CachedIcon> s_icons;

void scan_applications(StringView directory, Vector<AppInfo>& into)
{
    auto terminated = MUST(String::from_view(directory));
    DIR* listing = opendir(terminated.characters());
    if (!listing)
        return;
    while (auto* entry = readdir(listing)) {
        StringView name { entry->d_name, strlen(entry->d_name) };
        if (!name.ends_with(".app"_sv))
            continue;
        auto bundle = read_bundle(joined(directory, name).view());
        if (!bundle.is_error())
            into.append(bundle.release_value());
    }
    closedir(listing);
}

}

ErrorOr<AppInfo> read_bundle(StringView bundle_path)
{
    auto plist_path = joined(bundle_path, "Contents/Info.plist"_sv);
    auto bytes = TRY(read_whole_file(plist_path.view()));
    auto json = TRY(JSON::parse(StringView(reinterpret_cast<char const*>(bytes.data()), bytes.size())));
    AppInfo info;
    auto name = json.get_string("CFBundleName"_sv);
    auto identifier = json.get_string("CFBundleIdentifier"_sv);
    auto executable = json.get_string("CFBundleExecutable"_sv);
    if (!name.has_value() || !identifier.has_value() || !executable.has_value())
        return Error::from_errno(EINVAL);
    info.name = copy(name.value());
    info.identifier = copy(identifier.value());
    info.bundle_path = copy(bundle_path);
    StringBuilder path;
    path.append(bundle_path);
    path.append("/Contents/MacOS/"_sv);
    path.append(executable.value());
    info.executable_path = MUST(path.to_string());
    info.icon = copy(json.get_string("CFBundleIconFile"_sv).value_or("GenericApplication"_sv));
    info.kind = copy(json.get_string("VultureKind"_sv).value_or(StringView {}));
    info.category = copy(json.get_string("LSApplicationCategoryType"_sv).value_or(StringView {}));
    info.sandbox = copy(json.get_string("VultureSandbox"_sv).value_or(StringView {}));
    if (auto const* extensions = json.get_array("CFBundleDocumentExtensions"_sv)) {
        for (size_t i = 0; i < extensions->size(); ++i) {
            if (extensions->at(i).is_string())
                info.document_extensions.append(copy(extensions->at(i).as_string()));
        }
    }
    return info;
}

Vector<AppInfo> installed_applications()
{
    Vector<AppInfo> applications;
    scan_applications("/Applications"_sv, applications);
    scan_applications("/System/Applications"_sv, applications);
    // By name, as Launcher lists them.
    for (size_t i = 1; i < applications.size(); ++i) {
        for (size_t j = i; j > 0 && comes_after(applications[j - 1].name.view(), applications[j].name.view()); --j) {
            auto moved = move(applications[j]);
            applications[j] = move(applications[j - 1]);
            applications[j - 1] = move(moved);
        }
    }
    return applications;
}

Optional<AppInfo> application_with_identifier(StringView identifier)
{
    for (auto& application : installed_applications()) {
        if (application.identifier.view() == identifier)
            return Optional<AppInfo>(move(application));
    }
    StringView const core[] = { "/System/Library/CoreServices/Files.app"_sv };
    for (auto path : core) {
        auto bundle = read_bundle(path);
        if (!bundle.is_error() && bundle.value().identifier.view() == identifier)
            return bundle.release_value();
    }
    return {};
}

ErrorOr<pid_t> launch(AppInfo const& application)
{
    return launch(application, Span<StringView const> {});
}

static bool is_a_path(StringView argument)
{
    return argument.starts_with('/');
}

static String token_for(StringView path)
{
    auto terminated = copy(path);
    char* token = sandbox_extension_issue(terminated.characters(), 1);
    if (!token)
        return {};
    auto result = copy(StringView(token, strlen(token)));
    free(token);
    return result;
}

bool tell_running_application(AppInfo const& application, Span<StringView const> arguments)
{
    StringBuilder name;
    name.append("com.vulture.app."_sv);
    name.append(application.identifier.view());
    auto port = IPC::look_up(name.view());
    if (port.is_error())
        return false;
    AppInstanceProxy instance(port.release_value());
    Vector<String> given;
    Vector<String> tokens;
    for (auto argument : arguments) {
        given.append(copy(argument));
        tokens.append(is_a_path(argument) ? token_for(argument) : String {});
    }
    if (given.is_empty())
        return !instance.reopen().is_error();
    return !instance.open_arguments(given, tokens).is_error();
}

void perform_service(AppInfo const& application, StringView message, StringView text)
{
    StringBuilder name;
    name.append("com.vulture.app."_sv);
    name.append(application.identifier.view());
    auto port = IPC::look_up(name.view());
    if (!port.is_error()) {
        AppInstanceProxy instance(port.release_value());
        if (!instance.perform_service(message, text).is_error())
            return;
    }
    StringView arguments[3] = { "--service"_sv, message, text };
    (void)launch(application, Span<StringView const>(arguments, 3));
}

ErrorOr<pid_t> launch(AppInfo const& application, Span<StringView const> arguments)
{
    // An application only runs once.
    if (tell_running_application(application, arguments))
        return 0;
    note_recent_application(application.bundle_path.view());

    // What it's being given to open, it's let at, whatever its sandbox says otherwise.
    Vector<String> tokens;
    if (!application.sandbox.is_empty()) {
        for (auto argument : arguments) {
            if (is_a_path(argument))
                tokens.append(token_for(argument));
        }
    }

    Vector<String> owned;
    owned.append(copy(application.executable_path.view()));
    for (auto argument : arguments)
        owned.append(copy(argument));
    Vector<char*> argv;
    for (auto& argument : owned)
        argv.append(const_cast<char*>(argument.characters()));
    argv.append(nullptr);
    // Forked twice, so that it is nobody's child here: what launches an application does
    // not wait for it, and it would stay a zombie after it quit. The one in the middle
    // says who it started, and goes.
    int answer[2];
    if (pipe(answer) != 0)
        return Error::from_errno(errno);
    pid_t middle = fork();
    if (middle < 0) {
        int saved = errno;
        close(answer[0]);
        close(answer[1]);
        return Error::from_errno(saved);
    }
    if (middle == 0) {
        close(answer[0]);
        setsid();
        pid_t program = fork();
        if (program == 0) {
            close(answer[1]);
            if (!application.sandbox.is_empty() && sandbox_init_for_program(application.sandbox.characters(), SANDBOX_NAMED, argv[0], nullptr) != 0)
                _exit(126);
            for (auto const& token : tokens) {
                if (!token.is_empty())
                    (void)sandbox_extension_consume(token.characters());
            }
            execv(argv[0], argv.data());
            _exit(127);
        }
        (void)!write(answer[1], &program, sizeof(program));
        _exit(0);
    }
    close(answer[1]);
    pid_t program = -1;
    ssize_t got = read(answer[0], &program, sizeof(program));
    close(answer[0]);
    while (waitpid(middle, nullptr, 0) < 0 && errno == EINTR) { }
    if (got != sizeof(program) || program < 0)
        return Error::from_errno(EAGAIN);
    static os_log_t const log = os_log_create("com.vulture.desktop", "launch");
    os_log_info(log, "Launched %s as process %d", application.name.characters(), program);
    return program;
}

ErrorOr<pid_t> open_application(AppInfo const& application)
{
    auto port = TRY(IPC::look_up("com.vulture.workspace"_sv));
    auto reply = TRY(WorkspaceProxy(move(port)).open_application(application.bundle_path.view()));
    if (reply.error != 0)
        return Error::from_errno(reply.error);
    return static_cast<pid_t>(reply.pid);
}

ErrorOr<void> open_path_with(StringView path, StringView application_bundle)
{
    auto bundle = TRY(read_bundle(application_bundle));
    StringView arguments[1] = { path };
    (void)TRY(launch(bundle, Span<StringView const>(arguments, 1)));
    return {};
}

ErrorOr<void> reveal_in_files(StringView path)
{
    size_t at = path.length();
    while (at > 1 && path[at - 1] != '/')
        --at;
    // (A bundle's folder, not the bundle: that would run it.)
    auto folder = path.substring_view(0, at > 1 ? at - 1 : 1);
    auto port = IPC::look_up("com.vulture.Files"_sv);
    if (port.is_error())
        return Error::from_errno(ENOENT);
    (void)FilesProxy(port.release_value()).open_folder(folder);
    return {};
}

ErrorOr<void> open_path(StringView path)
{
    if (path.ends_with(".app"_sv)) {
        auto bundle = TRY(read_bundle(path));
        (void)TRY(launch(bundle));
        return {};
    }
    if (is_directory(path)) {
        // Files is one program, and says where it can be found.
        auto port = IPC::look_up("com.vulture.Files"_sv);
        if (!port.is_error()) {
            (void)FilesProxy(port.release_value()).open_folder(path);
            return {};
        }
        auto files = application_with_identifier("com.vulture.Files"_sv);
        if (!files.has_value())
            return Error::from_errno(ENOENT);
        StringView arguments[1] = { path };
        (void)TRY(launch(files.value(), Span<StringView const>(arguments, 1)));
        return {};
    }
    auto application = default_application_for_path(path);
    if (!application.has_value())
        return Error::from_errno(ENOENT);
    note_recent_document(path);
    StringView arguments[1] = { path };
    (void)TRY(launch(application.value(), Span<StringView const>(arguments, 1)));
    return {};
}

RefPtr<Bitmap> icon_named(StringView name, int points, int scale)
{
    for (auto const& cached : s_icons) {
        if (cached.points == points && cached.scale == scale && cached.name.view() == name)
            return cached.bitmap;
    }
    StringBuilder path;
    path.append("/System/Library/Icons/"_sv);
    path.append(name);
    path.append(".vicns"_sv);
    auto bytes = read_whole_file(path.view());
    if (bytes.is_error()) {
        if (name != "GenericApplication"_sv)
            return icon_named("GenericApplication"_sv, points, scale);
        return nullptr;
    }
    auto file = Image::IconFile::parse(bytes.value().span());
    if (file.is_error())
        return nullptr;
    auto bitmap = file.value().bitmap_for(static_cast<u32>(points), static_cast<u32>(scale));
    if (bitmap.is_error())
        return nullptr;
    RefPtr<Bitmap> result = bitmap.release_value();
    s_icons.append(CachedIcon { copy(name), points, scale, result });
    return result;
}

String icon_name_for_path(StringView path)
{
    if (path.ends_with(".app"_sv)) {
        auto bundle = read_bundle(path);
        if (!bundle.is_error())
            return copy(bundle.value().icon.view());
        return copy("GenericApplication"_sv);
    }
    if (is_directory(path))
        return copy("Folder"_sv);
    return copy("Document"_sv);
}

ErrorOr<ShellState> parse_shell_state(StringView text)
{
    auto json = TRY(JSON::parse(text));
    ShellState state;
    state.active_pid = static_cast<i32>(json.get_integer("active"_sv).value_or(0));
    state.active_name = copy(json.get_string("activeName"_sv).value_or(StringView {}));
    state.active_identifier = copy(json.get_string("activeIdentifier"_sv).value_or(StringView {}));
    state.menus = copy(json.get_string("menus"_sv).value_or(StringView {}));
    state.space = static_cast<int>(json.get_integer("space"_sv).value_or(0));
    state.space_count = static_cast<int>(json.get_integer("spaces"_sv).value_or(1));
    state.overview = json.get_boolean("overview"_sv).value_or(false);
    state.stage = json.get_boolean("stage"_sv).value_or(false);
    state.desktop_revealed = json.get_boolean("desktopRevealed"_sv).value_or(false);
    state.switcher_selected = static_cast<int>(json.get_integer("switcherSelected"_sv).value_or(0));
    if (auto const* switcher = json.get_array("switcher"_sv)) {
        for (size_t i = 0; i < switcher->size(); ++i)
            state.switcher.append(static_cast<i32>(switcher->at(i).as_integer()));
    }
    if (auto const* applications = json.get_array("applications"_sv)) {
        for (size_t i = 0; i < applications->size(); ++i) {
            auto const& one = applications->at(i);
            ApplicationEntry application;
            application.pid = static_cast<i32>(one.get_integer("pid"_sv).value_or(0));
            application.name = copy(one.get_string("name"_sv).value_or(StringView {}));
            application.identifier = copy(one.get_string("identifier"_sv).value_or(StringView {}));
            application.hidden = one.get_boolean("hidden"_sv).value_or(false);
            application.not_responding = one.get_boolean("notResponding"_sv).value_or(false);
            application.badge = copy(one.get_string("badge"_sv).value_or(StringView {}));
            if (auto const* windows = one.get_array("windows"_sv)) {
                for (size_t j = 0; j < windows->size(); ++j) {
                    auto const& window = windows->at(j);
                    WindowEntry entry;
                    entry.id = static_cast<u32>(window.get_integer("id"_sv).value_or(0));
                    entry.title = copy(window.get_string("title"_sv).value_or(StringView {}));
                    entry.minimized = window.get_boolean("minimized"_sv).value_or(false);
                    entry.space = static_cast<int>(window.get_integer("space"_sv).value_or(0));
                    application.windows.append(move(entry));
                }
            }
            state.applications.append(move(application));
        }
    }
    return state;
}

NonnullOwnPtr<UI::Menu> menu_from_json(JSON::Value const& description, Function<void(u32)> const& chosen)
{
    auto menu = adopt_own(*new UI::Menu(description.get_string("title"_sv).value_or(StringView {})));
    auto const* items = description.get_array("items"_sv);
    if (!items)
        return menu;
    for (size_t i = 0; i < items->size(); ++i) {
        auto const& item = items->at(i);
        if (item.get_boolean("separator"_sv).value_or(false)) {
            menu->add_separator();
            continue;
        }
        auto title = item.get_string("title"_sv).value_or(StringView {});
        if (item.get_array("items"_sv)) {
            auto& submenu = menu->add_submenu(title);
            auto built = menu_from_json(item, chosen);
            // (Moved over: a submenu is owned by its item.)
            for (auto& inner : built->items())
                (void)submenu.items().append(move(inner));
            built->items().clear();
            continue;
        }
        if (item.get_boolean("heading"_sv).value_or(false)) {
            (void)menu->add_heading(title);
            continue;
        }
        auto fill = [&chosen](UI::MenuItem& into, JSON::Value const& from) {
            auto id = static_cast<u32>(from.get_integer("id"_sv).value_or(0));
            into.action = [id, &chosen] {
                if (chosen)
                    chosen(id);
            };
            into.enabled = from.get_boolean("enabled"_sv).value_or(true);
            into.checked = from.get_boolean("checked"_sv).value_or(false);
            into.shortcut_text = copy(from.get_string("shortcut"_sv).value_or(StringView {}));
            if (auto const* parts = from.get_array("picture"_sv)) {
                for (size_t at = 0; at + 3 < parts->size(); at += 4)
                    into.picture.append({ static_cast<float>(parts->at(at).as_number()), static_cast<float>(parts->at(at + 1).as_number()), static_cast<float>(parts->at(at + 2).as_number()), static_cast<float>(parts->at(at + 3).as_number()) });
                into.palette = from.get_boolean("palette"_sv).value_or(false);
            }
        };
        auto& added = menu->add(title);
        fill(added, item);
        if (auto const* alternate = item.get_object("alternate"_sv)) {
            auto& other = UI::Menu::add_alternate(added, alternate->get_string("title"_sv).value_or(StringView {}), nullptr);
            fill(other, *alternate);
        }
    }
    return menu;
}

Preferences::Preferences(StringView identifier)
    : m_values(JSON::Value::object())
{
    StringBuilder path;
    path.append(home_directory().view());
    path.append("/Library/Preferences/"_sv);
    path.append(identifier);
    path.append(".json"_sv);
    m_path = MUST(path.to_string());
    auto bytes = read_whole_file(m_path.view());
    if (bytes.is_error())
        return;
    auto json = JSON::parse(StringView(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
    if (!json.is_error() && json.value().is_object())
        m_values = json.release_value();
}

void Preferences::set(StringView key, JSON::Value value)
{
    (void)m_values.set(key, move(value));
}

void Preferences::save()
{
    auto home = home_directory();
    auto library = joined(home.view(), "Library"_sv);
    auto directory = joined(library.view(), "Preferences"_sv);
    mkdir(library.characters(), 0755);
    mkdir(directory.characters(), 0755);
    auto text = m_values.serialize(true);
    if (text.is_error())
        return;
    int fd = open(m_path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    (void)write(fd, text.value().characters(), text.value().length());
    close(fd);
}

String home_directory()
{
    if (auto const* home = getenv("HOME"))
        return copy(StringView(home, strlen(home)));
    if (auto* entry = getpwuid(getuid()))
        return copy(StringView(entry->pw_dir, strlen(entry->pw_dir)));
    return copy("/"_sv);
}

void draw_logo(UI::Graphics& graphics, FloatRect const& shape, Color color)
{
    auto at = [&](float x, float y) { return FloatPoint { shape.x + x * shape.width, shape.y + y * shape.height }; };
    Path path;
    path.move_to(at(0.02f, 0.14f));
    path.cubic_to(at(0.16f, 0.30f), at(0.30f, 0.34f), at(0.40f, 0.42f));
    path.line_to(at(0.50f, 0.96f));
    path.line_to(at(0.60f, 0.42f));
    path.cubic_to(at(0.70f, 0.34f), at(0.84f, 0.30f), at(0.98f, 0.14f));
    path.cubic_to(at(0.90f, 0.52f), at(0.72f, 0.62f), at(0.60f, 0.70f));
    path.line_to(at(0.50f, 0.96f));
    path.line_to(at(0.40f, 0.70f));
    path.cubic_to(at(0.28f, 0.62f), at(0.10f, 0.52f), at(0.02f, 0.14f));
    path.close();
    graphics.painter().fill_path(path, color);
}

u32 post_notification(StringView application, StringView title, StringView body, Span<StringView const> actions)
{
    auto port = IPC::look_up("com.vulture.notificationd"_sv);
    if (port.is_error())
        return 0;
    NotificationServiceProxy center(port.release_value());
    Vector<String> titles;
    for (auto action : actions)
        titles.append(copy(action));
    auto id = center.post(application, title, body, titles);
    return id.is_error() ? 0 : id.value();
}

class NotificationResponses::Stub final : public NotificationResponsesStub {
public:
    explicit Stub(NotificationResponses& owner)
        : m_owner(owner)
    {
    }

    void responded(IPC::Sender const&, u32 id, String action) override
    {
        if (m_owner.on_response)
            m_owner.on_response(id, action.view());
    }

private:
    NotificationResponses& m_owner;
};

NotificationResponses::NotificationResponses(Core::EventLoop& loop, StringView application)
    : m_stub(adopt_own(*new Stub(*this)))
{
    auto port = IPC::look_up("com.vulture.notificationd"_sv);
    if (port.is_error())
        return;
    auto pair = IPC::create_port();
    if (pair.is_error())
        return;
    auto ports = pair.release_value();
    m_listener = adopt_own(*new IPC::Listener(loop, move(ports.receive), [this](IPC::ReceivedMessage& message) { (void)m_stub->handle(message); }));
    (void)NotificationServiceProxy(port.release_value()).listen(application, move(ports.send));
}

NotificationResponses::~NotificationResponses() = default;

void log_out()
{
    auto port = IPC::look_up("com.vulture.loginwindow"_sv);
    if (!port.is_error())
        (void)SessionProxy(port.release_value()).log_out();
}

void restart()
{
    auto port = IPC::look_up("com.vulture.loginwindow"_sv);
    if (!port.is_error())
        (void)SessionProxy(port.release_value()).restart();
}

void shut_down()
{
    auto port = IPC::look_up("com.vulture.loginwindow"_sv);
    if (!port.is_error())
        (void)SessionProxy(port.release_value()).shut_down();
}

void lock_screen()
{
    auto port = IPC::look_up("com.vulture.loginwindow"_sv);
    if (!port.is_error())
        (void)SessionProxy(port.release_value()).lock_screen();
}

void start_screen_saver()
{
    auto port = IPC::look_up("com.vulture.loginwindow"_sv);
    if (!port.is_error())
        (void)SessionProxy(port.release_value()).start_screen_saver();
}

void switch_to_user(StringView name)
{
    auto port = IPC::look_up("com.vulture.loginwindow"_sv);
    if (!port.is_error())
        (void)SessionProxy(port.release_value()).switch_to(name);
}

// A word's first letter, whatever it's written in: its first character, in capitals if it has them.
static void append_initial(StringBuilder& out, StringView word)
{
    if (word.is_empty())
        return;
    auto const first = static_cast<u8>(word[0]);
    if (first < 0x80) {
        out.append(first >= 'a' && first <= 'z' ? static_cast<char>(first - 'a' + 'A') : static_cast<char>(first));
        return;
    }
    size_t length = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : 2;
    out.append(word.substring_view(0, min(length, word.length())));
}

void draw_monogram(UI::Graphics& graphics, Gfx::FloatRect rect, StringView full_name)
{
    auto const& t = UI::Appearance::the();
    Gfx::Path circle;
    circle.add_circle({ rect.x + rect.width / 2, rect.y + rect.height / 2 }, rect.width / 2);
    Gfx::GradientStop const stops[] = { { 0, t.color("loginwindow.monogramTop"_sv) }, { 1, t.color("loginwindow.monogramBottom"_sv) } };
    graphics.painter().fill_path(circle, Gfx::Gradient::linear({ rect.x, rect.y }, { rect.x, rect.y + rect.height }, Span<Gfx::GradientStop const>(stops, 2)));
    // The first name's initial and the last's.
    StringView first, last;
    full_name.for_each_split_view(' ', SplitBehavior::SkipEmpty, [&](StringView word) {
        if (first.is_empty())
            first = word;
        else
            last = word;
    });
    StringBuilder initials;
    append_initial(initials, first);
    append_initial(initials, last);
    UI::FontSpec const font { rect.width * 0.4f, UI::FontWeight::Medium };
    graphics.draw_text_centered(initials.view(), font, rect, Gfx::Color::from_rgb(0xffffff));
}

Vector<String> login_items()
{
    Vector<String> items;
    Preferences preferences("com.vulture.loginitems"_sv);
    if (auto const* list = preferences.array("items"_sv)) {
        for (size_t i = 0; i < list->size(); ++i) {
            if (list->at(i).is_string())
                items.append(MUST(String::from_view(list->at(i).as_string())));
        }
    }
    return items;
}

bool is_login_item(StringView path)
{
    for (auto const& item : login_items()) {
        if (item.view() == path)
            return true;
    }
    return false;
}

void set_login_item(StringView path, bool opens)
{
    auto items = JSON::Value::array();
    bool there = false;
    for (auto const& item : login_items()) {
        if (item.view() == path) {
            there = true;
            if (!opens)
                continue;
        }
        MUST(items.append(MUST(JSON::Value::string(item.view()))));
    }
    if (opens && !there)
        MUST(items.append(MUST(JSON::Value::string(path))));
    Preferences preferences("com.vulture.loginitems"_sv);
    preferences.set("items"_sv, move(items));
    preferences.save();
}

Vector<String> logged_in_users()
{
    Vector<String> names;
    auto port = IPC::look_up("com.vulture.loginwindow"_sv);
    if (port.is_error())
        return names;
    auto answer = SessionProxy(port.release_value()).logged_in();
    if (answer.is_error())
        return names;
    answer.value().view().for_each_split_view('\n', SplitBehavior::SkipEmpty, [&](StringView name) {
        names.append(MUST(String::from_view(name)));
    });
    return names;
}

class MenuExtra::ExtraView final : public UI::View {
public:
    ExtraView(MenuExtra& owner, String name, Function<void(UI::Graphics&, Gfx::FloatRect, Gfx::Color)> draw)
        : m_owner(owner)
        , m_name(move(name))
        , m_draw(move(draw))
    {
    }

    void set_highlighted(bool highlighted)
    {
        m_highlighted = highlighted;
        set_needs_display();
    }

    void draw(UI::Graphics& graphics) override
    {
        auto const& t = UI::Appearance::the();
        Gfx::FloatRect const all { 0, 0, width(), height() };
        if (m_highlighted) {
            float const inset = t.metric("menubar.highlightInset"_sv);
            graphics.fill_rounded({ 0, inset / 2, width(), height() - inset }, t.metric("menubar.highlightRadius"_sv), t.color("menubar.highlight"_sv));
        }
        if (m_draw)
            m_draw(graphics, all, t.color("menubar.text"_sv));
    }

    bool mouse_down(UI::MouseEvent const& event) override
    {
        if (event.button != UI::WS::ButtonLeft)
            return false;
        if (m_owner.on_press)
            m_owner.on_press(m_owner.screen_rect());
        return false;
    }

    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Button; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_name.view())); }

private:
    MenuExtra& m_owner;
    String m_name;
    Function<void(UI::Graphics&, Gfx::FloatRect, Gfx::Color)> m_draw;
    bool m_highlighted { false };
};

MenuExtra::MenuExtra(StringView name, float width, Function<void(UI::Graphics&, Gfx::FloatRect, Gfx::Color)> draw)
{
    auto const& t = UI::Appearance::the();
    int const height = static_cast<int>(t.metric("menubar.height"_sv));
    m_window = adopt_own(*new UI::Window(name, { 0, 0, static_cast<int>(width), height }, UI::WS::WindowBare | UI::WS::WindowSticky));
    m_window->set_transparent(true);
    m_window->backend().set_level(UI::WS::LevelMenuBar);
    m_view = &m_window->root_view().add<ExtraView>(*this, MUST(String::from_view(name)), move(draw));
    m_view->set_frame({ 0, 0, width, static_cast<float>(height) });
}

MenuExtra::~MenuExtra() = default;

void MenuExtra::show()
{
    m_window->show();
    (void)UI::Application::the().connection().server().add_menu_extra(m_window->backend().system_id());
}

void MenuExtra::hide()
{
    m_window->hide();
}

void MenuExtra::set_highlighted(bool highlighted)
{
    m_view->set_highlighted(highlighted);
}

Gfx::IntRect MenuExtra::screen_rect() const
{
    return m_window->frame();
}

}
