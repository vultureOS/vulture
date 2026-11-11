/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Tags.h"
#include <LibJSON/JSON.h>
#include <errno.h>
#include <sys/xattr.h>

namespace Desktop {

namespace {

constexpr char const* attribute = "com.apple.metadata:_kMDItemUserTags";

// Apple's published system colours, as the theme has them.
constexpr TagColor tags[] = {
    { "Red"_sv, { 255, 59, 48, 255 } },
    { "Orange"_sv, { 255, 149, 0, 255 } },
    { "Yellow"_sv, { 255, 204, 0, 255 } },
    { "Green"_sv, { 40, 205, 65, 255 } },
    { "Blue"_sv, { 0, 122, 255, 255 } },
    { "Purple"_sv, { 175, 82, 222, 255 } },
    { "Gray"_sv, { 142, 142, 147, 255 } },
};

}

Span<TagColor const> standard_tags()
{
    return { tags, sizeof(tags) / sizeof(tags[0]) };
}

Optional<Gfx::Color> color_of_tag(StringView name)
{
    for (auto const& tag : tags) {
        if (tag.name == name)
            return tag.color;
    }
    return {};
}

Vector<String> tags_of(StringView path)
{
    Vector<String> found;
    auto terminated = MUST(String::from_view(path));
    char buffer[2048];
    ssize_t const length = getxattr(terminated.characters(), attribute, buffer, sizeof(buffer), 0, 0);
    if (length <= 0)
        return found;
    auto parsed = JSON::parse(StringView(buffer, static_cast<size_t>(length)));
    if (parsed.is_error() || !parsed.value().is_array())
        return found;
    for (size_t i = 0; i < parsed.value().size(); ++i) {
        if (parsed.value().at(i).is_string())
            found.append(MUST(String::from_view(parsed.value().at(i).as_string())));
    }
    return found;
}

ErrorOr<void> set_tags(StringView path, Span<String const> names)
{
    auto terminated = TRY(String::from_view(path));
    if (names.is_empty()) {
        if (removexattr(terminated.characters(), attribute, 0) != 0 && errno != ENOATTR)
            return Error::from_errno(errno);
        return {};
    }
    auto list = JSON::Value::array();
    for (auto const& name : names)
        TRY(list.append(TRY(JSON::Value::string(name.view()))));
    auto text = TRY(list.serialize());
    if (setxattr(terminated.characters(), attribute, text.characters(), text.length(), 0, 0) != 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<void> toggle_tag(StringView path, StringView tag)
{
    auto names = tags_of(path);
    bool had = false;
    names.remove_all_matching([&](String const& name) {
        bool const same = name.view() == tag;
        had = had || same;
        return same;
    });
    if (!had)
        names.append(TRY(String::from_view(tag)));
    return set_tags(path, names.span());
}

}
