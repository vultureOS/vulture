/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <ContentTypes.h>
#include <Desktop.h>
#include <LibIPC/Bootstrap.h>
#include <LibJSON/JSON.h>
#include <SearchEndpoint.h>
#include <Tags.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace FilesApp {

bool s_still;

String joined(StringView directory, StringView name)
{
    StringBuilder builder;
    builder.append(directory);
    if (!directory.ends_with('/'))
        builder.append("/"_sv);
    builder.append(name);
    return MUST(builder.to_string());
}

StringView last_component(StringView path)
{
    for (size_t i = path.length(); i > 0; --i) {
        if (path[i - 1] == '/' && i != path.length())
            return path.substring_view(i, path.length() - i);
    }
    return path;
}

String parent_of(StringView path)
{
    size_t end = path.length();
    while (end > 1 && path[end - 1] != '/')
        --end;
    if (end > 1)
        --end;
    return MUST(String::from_view(path.substring_view(0, end)));
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

bool sorts_before(StringView a, StringView b)
{
    size_t common = min(a.length(), b.length());
    for (size_t i = 0; i < common; ++i) {
        char x = lower(a[i]), y = lower(b[i]);
        if (x != y)
            return x < y;
    }
    return a.length() < b.length();
}

Optional<FileItem> item_at_path(StringView path, bool)
{
    FileItem item;
    item.name = MUST(String::from_view(last_component(path)));
    item.path = MUST(String::from_view(path));
    struct stat link {};
    if (lstat(item.path.characters(), &link) != 0)
        return {};
    item.is_alias = S_ISLNK(link.st_mode);
    struct stat info {};
    if (stat(item.path.characters(), &info) == 0) {
        item.is_directory = S_ISDIR(info.st_mode);
        item.size = static_cast<u64>(info.st_size);
        item.modified = static_cast<i64>(info.st_mtime);
        item.created = static_cast<i64>(info.st_ctime);
    }
    item.icon = Desktop::icon_name_for_path(item.path.view());
    if (!item.is_directory && item.size > 0 && item.size <= 16 * MiB) {
        auto type = Desktop::content_type_of_path(item.path.view());
        item.has_thumbnail = Desktop::content_type_conforms(type, "public.image"_sv) || Desktop::content_type_conforms(type, "public.text"_sv);
    }
    item.tags = Desktop::tags_of(path);
    return item;
}

bool is_smart_path(StringView path)
{
    return path.starts_with(smart_prefix);
}

StringView smart_name(StringView path)
{
    auto rest = path.substring_view(smart_prefix.length());
    auto bar = rest.find('|');
    return bar.has_value() ? rest.substring_view(0, bar.value()) : rest;
}

StringView smart_query(StringView path)
{
    auto rest = path.substring_view(smart_prefix.length());
    auto bar = rest.find('|');
    return bar.has_value() ? rest.substring_view(bar.value() + 1) : StringView {};
}

String recents_path()
{
    return MUST(String::from_view("smart:Recents|kMDItemFSContentChangeDate >= $time.today(-30) && kMDItemContentType != \"public.folder\" && kMDItemContentType != \"com.apple.application-bundle\""_sv));
}

String tag_path(StringView tag)
{
    StringBuilder path;
    path.appendff("smart:{}|kMDItemUserTags == \"{}\"", tag, tag);
    return MUST(path.to_string());
}

Optional<String> smart_path_from_file(StringView path)
{
    int fd = open(MUST(String::from_view(path)).characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    char buffer[8192];
    ssize_t const got = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (got <= 0)
        return {};
    auto parsed = JSON::parse(StringView(buffer, static_cast<size_t>(got)));
    if (parsed.is_error())
        return {};
    auto query = parsed.value().get_string("query"_sv);
    if (!query.has_value())
        return {};
    auto name = last_component(path);
    name = name.substring_view(0, name.length() - saved_search_extension.length());
    StringBuilder out;
    out.appendff("smart:{}|{}", name, query.value());
    return MUST(out.to_string());
}

ErrorOr<String> save_smart_folder(StringView name, StringView query)
{
    auto folder = joined(Desktop::home_directory().view(), "Library/Saved Searches"_sv);
    (void)mkdir(joined(Desktop::home_directory().view(), "Library"_sv).characters(), 0755);
    (void)mkdir(folder.characters(), 0755);
    StringBuilder file_name;
    file_name.appendff("{}{}", name, saved_search_extension);
    auto path = joined(folder.view(), file_name.view());
    auto contents = JSON::Value::object();
    TRY(contents.set("query"_sv, TRY(JSON::Value::string(query))));
    auto text = TRY(contents.serialize(true));
    int fd = open(path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return Error::from_errno(errno);
    (void)write(fd, text.characters(), text.length());
    close(fd);
    return path;
}

static Vector<FileItem> list_smart(StringView path)
{
    Vector<FileItem> items;
    auto port = IPC::look_up("com.vulture.indexd"_sv);
    if (port.is_error())
        return items;
    SearchProxy search(port.release_value());
    auto answer = search.find(smart_query(path), {}, false, 1000);
    if (answer.is_error() || !answer.value().error.is_empty())
        return items;
    auto parsed = JSON::parse(answer.value().json.view());
    if (parsed.is_error())
        return items;
    for (size_t i = 0; i < parsed.value().size(); ++i) {
        if (auto item = item_at_path(parsed.value().at(i).as_string()); item.has_value())
            items.append(item.release_value());
    }
    // Recents are by when; anything else, as a folder is.
    bool const by_date = smart_name(path) == "Recents"_sv;
    for (size_t i = 1; i < items.size(); ++i) {
        for (size_t j = i; j > 0; --j) {
            bool const swap_them = by_date ? items[j].modified > items[j - 1].modified : sorts_before(items[j].name.view(), items[j - 1].name.view());
            if (!swap_them)
                break;
            auto moved = move(items[j]);
            items[j] = move(items[j - 1]);
            items[j - 1] = move(moved);
        }
    }
    return items;
}

Vector<FileItem> list_folder(StringView path, bool show_hidden)
{
    if (is_smart_path(path))
        return list_smart(path);
    Vector<FileItem> items;
    auto terminated = MUST(String::from_view(path));
    auto const add_from = [&](StringView folder) {
        auto const folder_name = MUST(String::from_view(folder));
        DIR* directory = opendir(folder_name.characters());
        if (!directory)
            return;
        while (auto* entry = readdir(directory)) {
            StringView name { entry->d_name, strlen(entry->d_name) };
            if (name == "."_sv || name == ".."_sv)
                continue;
            if (!show_hidden && name.starts_with('.'))
                continue;
            auto const full = joined(folder, name);
            // And what's flagged as hidden, as most of what's at the top of the disk is.
            struct stat flags {};
            if (!show_hidden && lstat(full.characters(), &flags) == 0 && (flags.st_flags & UF_HIDDEN))
                continue;
            if (auto item = item_at_path(full.view()); item.has_value())
                items.append(item.release_value());
        }
        closedir(directory);
    };
    add_from(terminated.view());
    // As in a Mac's Finder, the applications that came with the system are among the others, wherever they really are.
    if (path == "/Applications"_sv)
        add_from("/System/Applications"_sv);
    for (size_t i = 1; i < items.size(); ++i) {
        for (size_t j = i; j > 0 && sorts_before(items[j].name.view(), items[j - 1].name.view()); --j) {
            auto moved = move(items[j]);
            items[j] = move(items[j - 1]);
            items[j - 1] = move(moved);
        }
    }
    return items;
}

String size_text(u64 bytes)
{
    StringBuilder text;
    if (bytes < 1000)
        text.appendff("{} bytes", bytes);
    else if (bytes < 1000 * 1000)
        text.appendff("{} KB", (bytes + 500) / 1000);
    else if (bytes < 1000 * 1000 * 1000)
        text.appendff("{} MB", (bytes + 500 * 1000) / (1000 * 1000));
    else
        text.appendff("{}.{} GB", bytes / (1000 * 1000 * 1000), (bytes / (100 * 1000 * 1000)) % 10);
    return MUST(text.to_string());
}

String date_text(i64 seconds)
{
    // (The same one every time in a test, as the clock in the menu bar is.)
    time_t when = s_still ? 1768470060 : static_cast<time_t>(seconds);
    tm parts;
    localtime_r(&when, &parts);
    static constexpr StringView months[] = { "Jan"_sv, "Feb"_sv, "Mar"_sv, "Apr"_sv, "May"_sv, "Jun"_sv, "Jul"_sv, "Aug"_sv, "Sep"_sv, "Oct"_sv, "Nov"_sv, "Dec"_sv };
    int const hour = parts.tm_hour % 12 == 0 ? 12 : parts.tm_hour % 12;
    StringBuilder text;
    text.appendff("{} {}, {} at {}:{:02} {}", months[parts.tm_mon], parts.tm_mday, parts.tm_year + 1900, hour, parts.tm_min, parts.tm_hour < 12 ? "AM"_sv : "PM"_sv);
    return MUST(text.to_string());
}

ErrorOr<void> copy_file(StringView from, StringView to)
{
    auto source_path = MUST(String::from_view(from));
    auto target_path = MUST(String::from_view(to));
    struct stat info;
    if (lstat(source_path.characters(), &info) != 0)
        return Error::from_errno(errno);
    if (S_ISDIR(info.st_mode)) {
        if (mkdir(target_path.characters(), info.st_mode & 07777) != 0 && errno != EEXIST)
            return Error::from_errno(errno);
        for (auto const& item : list_folder(from, true))
            TRY(copy_file(item.path.view(), joined(to, item.name.view()).view()));
        return {};
    }
    int in = open(source_path.characters(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return Error::from_errno(errno);
    int out = open(target_path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, info.st_mode & 07777);
    if (out < 0) {
        int saved = errno;
        close(in);
        return Error::from_errno(saved);
    }
    static char buffer[65536];
    ssize_t got;
    while ((got = read(in, buffer, sizeof(buffer))) > 0) {
        if (write(out, buffer, static_cast<size_t>(got)) != got) {
            int saved = errno;
            close(in);
            close(out);
            return Error::from_errno(saved);
        }
    }
    close(in);
    close(out);
    return {};
}

ErrorOr<void> remove_tree(StringView path)
{
    auto terminated = MUST(String::from_view(path));
    struct stat info;
    if (lstat(terminated.characters(), &info) != 0)
        return Error::from_errno(errno);
    if (S_ISDIR(info.st_mode)) {
        for (auto const& item : list_folder(path, true))
            TRY(remove_tree(item.path.view()));
        if (rmdir(terminated.characters()) != 0)
            return Error::from_errno(errno);
        return {};
    }
    if (unlink(terminated.characters()) != 0)
        return Error::from_errno(errno);
    return {};
}

bool exists(StringView path)
{
    auto terminated = MUST(String::from_view(path));
    struct stat info;
    return lstat(terminated.characters(), &info) == 0;
}

String free_name(StringView directory, StringView wanted)
{
    if (!exists(joined(directory, wanted).view()))
        return MUST(String::from_view(wanted));
    for (int i = 2;; ++i) {
        StringBuilder builder;
        builder.append(wanted);
        builder.appendff(" {}", i);
        auto candidate = MUST(builder.to_string());
        if (!exists(joined(directory, candidate.view()).view()))
            return candidate;
    }
}

ErrorOr<void> move_item(StringView from, StringView to)
{
    auto a = MUST(String::from_view(from));
    auto b = MUST(String::from_view(to));
    if (rename(a.characters(), b.characters()) == 0)
        return {};
    // Across volumes: copied, then gone.
    TRY(copy_file(from, to));
    return remove_tree(from);
}

String free_copy_name(StringView directory, StringView original)
{
    // Before any extension, unless it's a folder or a whole bundle.
    auto dot = original.find_last('.');
    struct stat information {};
    bool const is_folder = stat(joined(directory, original).characters(), &information) == 0 && S_ISDIR(information.st_mode);
    bool const has_extension = dot.has_value() && dot.value() > 0 && !is_folder;
    auto stem = has_extension ? original.substring_view(0, dot.value()) : original;
    auto extension = has_extension ? original.substring_view(dot.value()) : StringView {};
    for (int i = 1;; ++i) {
        StringBuilder candidate;
        candidate.append(stem);
        candidate.append(" copy"_sv);
        if (i > 1)
            candidate.appendff(" {}", i);
        candidate.append(extension);
        auto name = MUST(candidate.to_string());
        if (!exists(joined(directory, name.view()).view()))
            return name;
    }
}

}
