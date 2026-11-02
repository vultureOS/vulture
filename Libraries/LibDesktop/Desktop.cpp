/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <os/log.h>
#include "Desktop.h"
#include <LibIPC/Bootstrap.h>
#include <LibImage/Image.h>
#include <LibUI/Application.h>
#include <FilesEndpoint.h>
#include <NotificationsEndpoint.h>
#include <SessionEndpoint.h>
#include <dirent.h>
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

StringView extension_of(StringView path)
{
    for (size_t i = path.length(); i > 0; --i) {
        if (path[i - 1] == '.')
            return path.substring_view(i, path.length() - i);
        if (path[i - 1] == '/')
            break;
    }
    return {};
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

ErrorOr<pid_t> launch(AppInfo const& application, Span<StringView const> arguments)
{
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
    auto extension = extension_of(path);
    for (auto const& application : installed_applications()) {
        for (auto const& handled : application.document_extensions) {
            if (handled.view() == extension) {
                StringView arguments[1] = { path };
                (void)TRY(launch(application, Span<StringView const>(arguments, 1)));
                return {};
            }
        }
    }
    return Error::from_errno(ENOENT);
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
    if (auto const* applications = json.get_array("applications"_sv)) {
        for (size_t i = 0; i < applications->size(); ++i) {
            auto const& one = applications->at(i);
            ApplicationEntry application;
            application.pid = static_cast<i32>(one.get_integer("pid"_sv).value_or(0));
            application.name = copy(one.get_string("name"_sv).value_or(StringView {}));
            application.identifier = copy(one.get_string("identifier"_sv).value_or(StringView {}));
            application.hidden = one.get_boolean("hidden"_sv).value_or(false);
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
        auto id = static_cast<u32>(item.get_integer("id"_sv).value_or(0));
        auto& added = menu->add(title, [id, &chosen] {
            if (chosen)
                chosen(id);
        });
        added.enabled = item.get_boolean("enabled"_sv).value_or(true);
        added.checked = item.get_boolean("checked"_sv).value_or(false);
        added.shortcut_text = copy(item.get_string("shortcut"_sv).value_or(StringView {}));
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

void post_notification(StringView application, StringView title, StringView body)
{
    auto port = IPC::look_up("com.vulture.notificationd"_sv);
    if (port.is_error())
        return;
    NotificationServiceProxy center(port.release_value());
    (void)center.post(application, title, body);
}

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

}
