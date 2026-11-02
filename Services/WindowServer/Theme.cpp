/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Theme.h"
#include <LibJSON/JSON.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace WindowServer {

namespace {

ErrorOr<Gfx::Color> parse_color(StringView text)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    if ((text.length() != 7 && text.length() != 9) || text[0] != '#')
        return Error::from_string_literal("theme: a color is not #rrggbb or #rrggbbaa");
    u8 values[4] = { 0, 0, 0, 255 };
    for (size_t i = 0; i < (text.length() - 1) / 2; ++i) {
        int high = hex(text[1 + i * 2]);
        int low = hex(text[2 + i * 2]);
        if (high < 0 || low < 0)
            return Error::from_string_literal("theme: a color is not #rrggbb or #rrggbbaa");
        values[i] = static_cast<u8>(high * 16 + low);
    }
    return Gfx::Color { values[0], values[1], values[2], values[3] };
}

ErrorOr<Gfx::Color> color_in(JSON::Value const& object, StringView key)
{
    auto text = object.get_string(key);
    if (!text.has_value())
        return Error::from_string_literal("theme: a color is missing");
    return parse_color(text.value());
}

ErrorOr<float> number_in(JSON::Value const& object, StringView key)
{
    auto value = object.get_number(key);
    if (!value.has_value())
        return Error::from_string_literal("theme: a number is missing");
    return static_cast<float>(value.value());
}

ErrorOr<JSON::Value const*> object_in(JSON::Value const& object, StringView key)
{
    auto const* found = object.get_object(key);
    if (!found)
        return Error::from_string_literal("theme: a section is missing");
    return found;
}

ErrorOr<ShadowStyle> shadow_in(JSON::Value const& object, StringView key)
{
    auto const* shadow = TRY(object_in(object, key));
    return ShadowStyle { TRY(number_in(*shadow, "blur"_sv)), TRY(number_in(*shadow, "offsetY"_sv)), TRY(color_in(*shadow, "color"_sv)) };
}

ErrorOr<MaterialStyle> material_in(JSON::Value const& object, StringView key)
{
    auto const* material = TRY(object_in(object, key));
    return MaterialStyle { TRY(number_in(*material, "blur"_sv)), TRY(number_in(*material, "saturation"_sv)), TRY(color_in(*material, "tint"_sv)) };
}

}

ErrorOr<Theme> load_theme(StringView path)
{
    char terminated[256] = {};
    if (path.length() >= sizeof(terminated))
        return Error::from_errno(ENAMETOOLONG);
    __builtin_memcpy(terminated, path.characters_without_null_termination(), path.length());
    int fd = open(terminated, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    u8 chunk[4096];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) {
        size_t at = bytes.size();
        TRY(bytes.try_resize(at + static_cast<size_t>(got)));
        __builtin_memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    auto root = TRY(JSON::parse(StringView(reinterpret_cast<char const*>(bytes.data()), bytes.size())));

    Theme theme;
    auto const* wallpaper = root.get_array("wallpaper"_sv);
    if (!wallpaper || wallpaper->size() == 0)
        return Error::from_string_literal("theme: no wallpaper colors");
    for (size_t i = 0; i < wallpaper->size(); ++i) {
        if (!wallpaper->at(i).is_string())
            return Error::from_string_literal("theme: a wallpaper color is not a string");
        TRY(theme.wallpaper.try_append(TRY(parse_color(wallpaper->at(i).as_string()))));
    }
    auto const* window = TRY(object_in(root, "window"_sv));
    theme.corner_radius = TRY(number_in(*window, "cornerRadius"_sv));
    theme.window_background = TRY(color_in(*window, "background"_sv));
    theme.outline = TRY(color_in(*window, "outline"_sv));
    auto const* title = TRY(object_in(*window, "titleBar"_sv));
    theme.title_bar_height = static_cast<int>(TRY(number_in(*title, "height"_sv)));
    theme.title_bar_background = TRY(color_in(*title, "background"_sv));
    theme.title_bar_background_inactive = TRY(color_in(*title, "backgroundInactive"_sv));
    theme.title_bar_separator = TRY(color_in(*title, "separator"_sv));
    theme.title_bar_highlight = TRY(color_in(*title, "highlight"_sv));
    theme.title_bar_highlight_inactive = TRY(color_in(*title, "highlightInactive"_sv));
    theme.title_text = TRY(color_in(*title, "text"_sv));
    theme.title_text_inactive = TRY(color_in(*title, "textInactive"_sv));
    theme.title_font_size = TRY(number_in(*title, "fontSize"_sv));
    auto const* lights = TRY(object_in(*window, "trafficLights"_sv));
    theme.traffic_light_diameter = TRY(number_in(*lights, "diameter"_sv));
    theme.traffic_light_spacing = TRY(number_in(*lights, "spacing"_sv));
    theme.traffic_light_inset = TRY(number_in(*lights, "inset"_sv));
    theme.close = TRY(color_in(*lights, "close"_sv));
    theme.close_border = TRY(color_in(*lights, "closeBorder"_sv));
    theme.minimize = TRY(color_in(*lights, "minimize"_sv));
    theme.minimize_border = TRY(color_in(*lights, "minimizeBorder"_sv));
    theme.zoom = TRY(color_in(*lights, "zoom"_sv));
    theme.zoom_border = TRY(color_in(*lights, "zoomBorder"_sv));
    theme.traffic_light_inactive = TRY(color_in(*lights, "inactive"_sv));
    theme.traffic_light_inactive_border = TRY(color_in(*lights, "inactiveBorder"_sv));
    theme.traffic_light_glyph = TRY(color_in(*lights, "glyph"_sv));
    auto const* shadow = TRY(object_in(*window, "shadow"_sv));
    theme.shadow_active = TRY(shadow_in(*shadow, "active"_sv));
    theme.shadow_inactive = TRY(shadow_in(*shadow, "inactive"_sv));
    theme.shadow_popup = TRY(shadow_in(*shadow, "popup"_sv));
    auto const* desktop = TRY(object_in(root, "desktop"_sv));
    theme.minimize_seconds = TRY(number_in(*desktop, "minimizeSeconds"_sv));
    theme.space_slide_seconds = TRY(number_in(*desktop, "spaceSlideSeconds"_sv));
    auto const* mission = TRY(object_in(*desktop, "overview"_sv));
    theme.overview_seconds = TRY(number_in(*mission, "seconds"_sv));
    theme.overview_bar = TRY(number_in(*mission, "bar"_sv));
    theme.overview_padding = TRY(number_in(*mission, "padding"_sv));
    theme.overview_tile_width = TRY(number_in(*mission, "tileWidth"_sv));
    theme.overview_tile_height = TRY(number_in(*mission, "tileHeight"_sv));
    theme.overview_tile_radius = TRY(number_in(*mission, "tileRadius"_sv));
    theme.overview_dim = TRY(color_in(*mission, "dim"_sv));
    theme.overview_tile = TRY(color_in(*mission, "tile"_sv));
    theme.overview_tile_active = TRY(color_in(*mission, "tileActive"_sv));
    theme.overview_highlight = TRY(color_in(*mission, "highlight"_sv));
    auto const* boot = TRY(object_in(*desktop, "boot"_sv));
    theme.boot_seconds = TRY(number_in(*boot, "seconds"_sv));
    theme.boot_bar_width = TRY(number_in(*boot, "barWidth"_sv));
    theme.boot_bar_height = TRY(number_in(*boot, "barHeight"_sv));
    theme.boot_logo = TRY(number_in(*boot, "logo"_sv));
    theme.boot_background = TRY(color_in(*boot, "background"_sv));
    theme.boot_foreground = TRY(color_in(*boot, "foreground"_sv));
    theme.boot_track = TRY(color_in(*boot, "track"_sv));
    auto const* materials = TRY(object_in(root, "materials"_sv));
    using Window::Material;
    theme.materials[static_cast<int>(Material::MenuBar)] = TRY(material_in(*materials, "menuBar"_sv));
    theme.materials[static_cast<int>(Material::Popover)] = TRY(material_in(*materials, "popover"_sv));
    theme.materials[static_cast<int>(Material::Sidebar)] = TRY(material_in(*materials, "sidebar"_sv));
    theme.materials[static_cast<int>(Material::HUD)] = TRY(material_in(*materials, "hud"_sv));
    theme.materials[static_cast<int>(Material::UnderWindow)] = TRY(material_in(*materials, "underWindow"_sv));
    return theme;
}

}
