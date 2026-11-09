/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Documents.h"
#include "Desktop.h"
#include <LibBase/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <SearchEndpoint.h>
#include <WorkspaceEndpoint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sandbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace Desktop {

namespace {

constexpr StringView recents_identifier = "com.vulture.recentitems"_sv;
constexpr size_t most_recent_items = 10;
constexpr size_t most_versions = 64;

String copy_of(StringView text)
{
    return MUST(String::from_view(text));
}

String in_home(StringView relative)
{
    StringBuilder path;
    path.append(home_directory().view());
    path.append('/');
    path.append(relative);
    return MUST(path.to_string());
}

void make_directories(StringView path)
{
    for (size_t i = 1; i <= path.length(); ++i) {
        if (i == path.length() || path[i] == '/')
            (void)mkdir(copy_of(path.substring_view(0, i)).characters(), 0700);
    }
}

ErrorOr<String> read_file(StringView path)
{
    int fd = open(copy_of(path).characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    StringBuilder text;
    char buffer[16384];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            int saved = errno;
            close(fd);
            return Error::from_errno(saved);
        }
        if (got == 0)
            break;
        text.append(StringView(buffer, static_cast<size_t>(got)));
    }
    close(fd);
    return text.to_string();
}

Vector<String> strings_in(JSON::Value const* array)
{
    Vector<String> out;
    if (!array)
        return out;
    for (size_t i = 0; i < array->size(); ++i) {
        if (array->at(i).is_string())
            out.append(copy_of(array->at(i).as_string()));
    }
    return out;
}

void tell_workspace_of_recent(StringView path, bool is_application)
{
    auto port = IPC::look_up("com.vulture.workspace"_sv);
    if (port.is_error())
        return;
    WorkspaceProxy workspace(port.release_value());
    (void)workspace.note_recent(path, is_application);
}

}

void note_recent_document(StringView path)
{
    tell_workspace_of_recent(path, false);
}

void note_recent_application(StringView bundle_path)
{
    tell_workspace_of_recent(bundle_path, true);
}

void add_recent_item(StringView path, bool is_application)
{
    Preferences preferences(recents_identifier);
    auto key = is_application ? "applications"_sv : "documents"_sv;
    auto items = strings_in(preferences.array(key));
    items.remove_all_matching([&](String const& item) { return item.view() == path; });
    items.insert(0, copy_of(path));
    auto list = JSON::Value::array();
    for (size_t i = 0; i < items.size() && i < most_recent_items; ++i)
        MUST(list.append(MUST(JSON::Value::string(items[i].view()))));
    preferences.set(key, move(list));
    preferences.save();
}

Vector<String> recent_documents()
{
    Preferences preferences(recents_identifier);
    return strings_in(preferences.array("documents"_sv));
}

Vector<String> recent_applications()
{
    Preferences preferences(recents_identifier);
    return strings_in(preferences.array("applications"_sv));
}

void clear_recent_items()
{
    Preferences preferences(recents_identifier);
    preferences.set("documents"_sv, JSON::Value::array());
    preferences.set("applications"_sv, JSON::Value::array());
    preferences.save();
}

ErrorOr<void> write_file_atomically(StringView path, StringView contents)
{
    StringBuilder temporary_builder;
    temporary_builder.append(path);
    temporary_builder.appendff(".{}.saving", getpid());
    auto temporary = TRY(temporary_builder.to_string());
    auto final_path = TRY(String::from_view(path));
    int fd = open(temporary.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return Error::from_errno(errno);
    size_t done = 0;
    while (done < contents.length()) {
        ssize_t wrote = write(fd, contents.characters_without_null_termination() + done, contents.length() - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            unlink(temporary.characters());
            return Error::from_errno(saved);
        }
        done += static_cast<size_t>(wrote);
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(temporary.characters(), final_path.characters()) != 0) {
        int saved = errno;
        unlink(temporary.characters());
        return Error::from_errno(saved);
    }
    return {};
}

// ---- Panels.

namespace {

// One question, asked of the desktop.
class RemotePanel final : public FilePanelRepliesStub {
public:
    explicit RemotePanel(Function<void(StringView)> then)
        : m_then(move(then))
    {
    }

    bool start(UI::FilePanel::Kind kind, StringView directory, StringView suggested)
    {
        auto workspace = IPC::look_up("com.vulture.workspace"_sv);
        auto pair = IPC::create_port();
        if (workspace.is_error() || pair.is_error())
            return false;
        auto ports = pair.release_value();
        m_listener = adopt_own(*new IPC::Listener(Core::EventLoop::current(), move(ports.receive), [this](IPC::ReceivedMessage& message) { (void)handle(message); }));
        WorkspaceProxy service(workspace.release_value());
        return !service.show_file_panel(0, static_cast<u32>(kind), directory, suggested, move(ports.send)).is_error();
    }

    void chosen(IPC::Sender const&, u32, String path, String token) override
    {
        if (!token.is_empty())
            (void)sandbox_extension_consume(token.characters());
        auto then = move(m_then);
        auto* self = this;
        Core::EventLoop::current().deferred_invoke([self] { delete self; });
        if (then)
            then(path.view());
    }

private:
    Function<void(StringView)> m_then;
    OwnPtr<IPC::Listener> m_listener;
};

}

void use_search_in_file_panels()
{
    UI::FilePanel::set_search([](StringView text) -> Optional<Vector<String>> {
        auto port = IPC::look_up("com.vulture.indexd"_sv);
        if (port.is_error())
            return {};
        SearchProxy search(port.release_value());
        auto answer = search.query(text, 64);
        if (answer.is_error())
            return {};
        auto parsed = JSON::parse(answer.value().view());
        if (parsed.is_error())
            return {};
        Vector<String> paths;
        for (size_t i = 0; i < parsed.value().size(); ++i)
            paths.append(copy_of(parsed.value().at(i).get_string("path"_sv).value_or(StringView {})));
        return paths;
    });
}

void run_file_panel(UI::Window& window, UI::FilePanel::Kind kind, StringView directory, StringView suggested_name, Function<void(StringView)> then)
{
    use_search_in_file_panels();
    // Somewhere it can see for itself: then it's an ordinary panel.
    int const folder = open(copy_of(directory).characters(), O_RDONLY | O_CLOEXEC);
    if (folder >= 0) {
        close(folder);
        UI::FilePanel::run(window, kind, directory, suggested_name, move(then));
        return;
    }
    auto* panel = new RemotePanel(move(then));
    if (!panel->start(kind, directory, suggested_name))
        delete panel;
}

// ---- Versions.

DocumentVersions::DocumentVersions(StringView document_path)
{
    StringBuilder relative;
    relative.append("Library/Application Support/Document Revisions/"_sv);
    relative.appendff("{:016x}", Base::hash_bytes(document_path.characters_without_null_termination(), document_path.length()));
    m_folder = in_home(relative.view());
}

Vector<DocumentVersion> DocumentVersions::list() const
{
    Vector<DocumentVersion> versions;
    StringBuilder index;
    index.append(m_folder.view());
    index.append("/index.json"_sv);
    auto text = read_file(index.view());
    if (text.is_error())
        return versions;
    auto parsed = JSON::parse(text.value().view());
    if (parsed.is_error() || !parsed.value().is_array())
        return versions;
    for (size_t i = 0; i < parsed.value().size(); ++i) {
        auto const& one = parsed.value().at(i);
        DocumentVersion version;
        version.file = copy_of(one.get_string("file"_sv).value_or(StringView {}));
        version.time = one.get_integer("time"_sv).value_or(0);
        version.label = copy_of(one.get_string("label"_sv).value_or(StringView {}));
        versions.append(move(version));
    }
    return versions;
}

void DocumentVersions::write_index(Vector<DocumentVersion> const& versions) const
{
    auto list = JSON::Value::array();
    for (auto const& version : versions) {
        auto one = JSON::Value::object();
        MUST(one.set("file"_sv, MUST(JSON::Value::string(version.file.view()))));
        MUST(one.set("time"_sv, JSON::Value(version.time)));
        MUST(one.set("label"_sv, MUST(JSON::Value::string(version.label.view()))));
        MUST(list.append(move(one)));
    }
    StringBuilder index;
    index.append(m_folder.view());
    index.append("/index.json"_sv);
    (void)write_file_atomically(index.view(), MUST(list.serialize()).view());
}

ErrorOr<void> DocumentVersions::add(StringView contents, StringView label)
{
    make_directories(m_folder.view());
    auto versions = list();
    if (!versions.is_empty()) {
        auto newest = read(versions[0]);
        if (!newest.is_error() && newest.value().view() == contents)
            return {};
    }
    i64 const now = static_cast<i64>(time(nullptr));
    DocumentVersion version;
    StringBuilder name;
    name.appendff("{}-{}", now, versions.size());
    version.file = TRY(name.to_string());
    version.time = now;
    version.label = TRY(String::from_view(label));
    StringBuilder path;
    path.append(m_folder.view());
    path.append('/');
    path.append(version.file.view());
    TRY(write_file_atomically(path.view(), contents));
    versions.insert(0, move(version));
    while (versions.size() > most_versions) {
        StringBuilder old;
        old.append(m_folder.view());
        old.append('/');
        old.append(versions.last().file.view());
        unlink(MUST(old.to_string()).characters());
        versions.remove(versions.size() - 1);
    }
    write_index(versions);
    return {};
}

Optional<DocumentVersion> DocumentVersions::newest_labelled(StringView label) const
{
    for (auto const& version : list()) {
        if (version.label.view() == label)
            return version;
    }
    return {};
}

ErrorOr<String> DocumentVersions::read(DocumentVersion const& version) const
{
    StringBuilder path;
    path.append(m_folder.view());
    path.append('/');
    path.append(version.file.view());
    return read_file(path.view());
}

String new_autosave_path()
{
    auto folder = in_home("Library/Autosave Information"_sv);
    make_directories(folder.view());
    static unsigned serial = 0;
    StringBuilder path;
    path.appendff("{}/Untitled-{}-{}-{}.autosave", folder.view(), static_cast<i64>(time(nullptr)), getpid(), serial++);
    return MUST(path.to_string());
}

// ---- Saved state.

SavedState::SavedState(StringView identifier)
{
    StringBuilder relative;
    relative.append("Library/Saved Application State/"_sv);
    relative.append(identifier);
    relative.append(".json"_sv);
    m_path = in_home(relative.view());
}

Optional<JSON::Value> SavedState::load() const
{
    auto text = read_file(m_path.view());
    if (text.is_error())
        return {};
    auto parsed = JSON::parse(text.value().view());
    if (parsed.is_error())
        return {};
    return parsed.release_value();
}

void SavedState::save(JSON::Value const& state) const
{
    make_directories(in_home("Library/Saved Application State"_sv).view());
    (void)write_file_atomically(m_path.view(), MUST(state.serialize()).view());
}

void SavedState::clear() const
{
    unlink(m_path.characters());
}

bool asks_to_keep_changes()
{
    Preferences preferences("com.vulture.dock"_sv);
    return preferences.boolean("askToKeepChanges"_sv).value_or(false);
}

bool closes_windows_when_quitting()
{
    Preferences preferences("com.vulture.dock"_sv);
    return preferences.boolean("closeWindowsWhenQuitting"_sv).value_or(true);
}

}
