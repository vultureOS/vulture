/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ContentTypes.h>
#include <LibJSON/JSON.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Desktop {

namespace {

constexpr StringView registry_path = "/System/Library/CoreServices/ContentTypes.json"_sv;
constexpr StringView choices_identifier = "com.vulture.launchservices"_sv;

Vector<ContentType>& registry()
{
    static Vector<ContentType> types;
    static bool loaded = false;
    if (loaded)
        return types;
    loaded = true;
    int fd = open(MUST(String::from_view(registry_path)).characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return types;
    StringBuilder text;
    char buffer[8192];
    ssize_t got;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0)
        text.append(StringView(buffer, static_cast<size_t>(got)));
    close(fd);
    auto parsed = JSON::parse(text.view());
    if (parsed.is_error())
        return types;
    auto const* list = parsed.value().get_array("types"_sv);
    if (!list)
        return types;
    auto strings_of = [](JSON::Value const& value, StringView key, Vector<String>& out) {
        if (auto const* array = value.get_array(key)) {
            for (size_t i = 0; i < array->size(); ++i) {
                if (array->at(i).is_string())
                    out.append(MUST(String::from_view(array->at(i).as_string())));
            }
        }
    };
    for (size_t i = 0; i < list->size(); ++i) {
        auto const& one = list->at(i);
        ContentType type;
        type.identifier = MUST(String::from_view(one.get_string("identifier"_sv).value_or(StringView {})));
        type.description = MUST(String::from_view(one.get_string("description"_sv).value_or(StringView {})));
        type.icon = MUST(String::from_view(one.get_string("icon"_sv).value_or(StringView {})));
        strings_of(one, "extensions"_sv, type.extensions);
        strings_of(one, "conformsTo"_sv, type.conforms_to);
        types.append(move(type));
    }
    return types;
}

StringView extension_of(StringView path)
{
    auto slash = path.find_last('/');
    auto name = slash.has_value() ? path.substring_view(slash.value() + 1) : path;
    auto dot = name.find_last('.');
    if (!dot.has_value() || dot.value() == 0)
        return {};
    return name.substring_view(dot.value() + 1);
}

}

ContentType const* content_type_named(StringView identifier)
{
    for (auto const& type : registry()) {
        if (type.identifier.view() == identifier)
            return &type;
    }
    return nullptr;
}

ContentType const* content_type_for_extension(StringView extension)
{
    if (extension.is_empty())
        return nullptr;
    for (auto const& type : registry()) {
        for (auto const& candidate : type.extensions) {
            if (candidate.view().equals_ignoring_ascii_case(extension))
                return &type;
        }
    }
    return nullptr;
}

StringView content_type_of_path(StringView path)
{
    auto terminated = MUST(String::from_view(path));
    struct stat information;
    bool const exists = lstat(terminated.characters(), &information) == 0;
    if (exists && S_ISLNK(information.st_mode))
        return "public.symlink"_sv;
    if (exists && S_ISDIR(information.st_mode)) {
        if (path.ends_with(".app"_sv))
            return "com.apple.application-bundle"_sv;
        return "public.folder"_sv;
    }
    if (auto const* type = content_type_for_extension(extension_of(path)))
        return type->identifier.view();
    if (exists && S_ISREG(information.st_mode) && (information.st_mode & 0100))
        return "public.executable"_sv;
    return "public.data"_sv;
}

StringView content_description_of_path(StringView path)
{
    if (auto const* type = content_type_named(content_type_of_path(path)))
        return type->description.view();
    return "Document"_sv;
}

bool content_type_conforms(StringView type, StringView to)
{
    if (type == to)
        return true;
    auto const* information = content_type_named(type);
    if (!information)
        return false;
    for (auto const& parent : information->conforms_to) {
        if (content_type_conforms(parent.view(), to))
            return true;
    }
    return false;
}

Vector<AppInfo> applications_for_path(StringView path)
{
    auto extension = extension_of(path);
    auto type = content_type_of_path(path);
    Vector<AppInfo> claiming;
    if (type == "public.folder"_sv || type == "com.apple.application-bundle"_sv)
        return claiming;
    for (auto& application : installed_applications()) {
        bool claims = false;
        for (auto const& handled : application.document_extensions) {
            if (!extension.is_empty() && handled.view().equals_ignoring_ascii_case(extension))
                claims = true;
            // An extension of anything that's the same type counts as well: .text as well as .txt.
            if (auto const* other = content_type_for_extension(handled.view()); other && other->identifier.view() == type)
                claims = true;
        }
        if (claims)
            claiming.append(move(application));
    }
    Preferences choices(choices_identifier);
    if (auto chosen = choices.string(type); chosen.has_value()) {
        for (size_t i = 0; i < claiming.size(); ++i) {
            if (claiming[i].identifier.view() == chosen.value() && i != 0) {
                auto moved = move(claiming[i]);
                claiming.remove(i);
                claiming.insert(0, move(moved));
                break;
            }
        }
    }
    return claiming;
}

Optional<AppInfo> default_application_for_path(StringView path)
{
    auto all = applications_for_path(path);
    if (all.is_empty())
        return {};
    return move(all[0]);
}

void set_default_application(StringView content_type, StringView application_identifier)
{
    Preferences choices(choices_identifier);
    choices.set(content_type, MUST(JSON::Value::string(application_identifier)));
    choices.save();
}

}
