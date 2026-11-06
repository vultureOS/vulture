/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibGfx/Color.h>
#include <LibWindow/Protocol.h>

// How things look: nothing visual is written down in the server's code. These
// come from /System/Library/Themes/{Light,Dark}.json.

namespace WindowServer {

struct ShadowStyle {
    float blur { 0 };     // Standard deviation, in points.
    float offset_y { 0 }; // Points.
    Gfx::Color color;
};

struct MaterialStyle {
    float blur { 0 }; // Standard deviation, in points.
    float saturation { 1 };
    Gfx::Color tint;
};

struct Theme {
    Vector<Gfx::Color> wallpaper;
    float corner_radius { 0 };
    Gfx::Color window_background;
    int title_bar_height { 0 };
    Gfx::Color title_bar_background;
    Gfx::Color title_bar_background_inactive;
    Gfx::Color title_bar_separator;
    // A point's worth of light along the window's top edge.
    Gfx::Color title_bar_highlight;
    Gfx::Color title_bar_highlight_inactive;
    Gfx::Color title_text;
    Gfx::Color title_text_inactive;
    float title_font_size { 0 };
    float traffic_light_diameter { 0 };
    float traffic_light_spacing { 0 };
    float traffic_light_inset { 0 };
    Gfx::Color close, close_border, minimize, minimize_border, zoom, zoom_border;
    Gfx::Color traffic_light_inactive, traffic_light_inactive_border, traffic_light_glyph;
    Gfx::Color outline;
    ShadowStyle shadow_active;
    ShadowStyle shadow_inactive;
    ShadowStyle shadow_popup;
    MaterialStyle materials[static_cast<int>(Window::Material::Count)];
    // Motion and the desktop's overlays.
    float minimize_seconds { 0 };
    float space_slide_seconds { 0 };
    float overview_seconds { 0 };
    float overview_bar { 0 };
    // Its row of Spaces, only their names until the pointer goes up to it; and how long opening it takes.
    float overview_collapsed_bar { 0 };
    float overview_bar_seconds { 0 };
    float overview_padding { 0 };
    float tile_margin { 0 };
    Gfx::Color tile_preview;
    Gfx::Color tile_preview_border;
    float overview_tile_width { 0 };
    float overview_tile_height { 0 };
    float overview_tile_radius { 0 };
    Gfx::Color overview_dim;
    Gfx::Color overview_tile;
    Gfx::Color overview_tile_active;
    Gfx::Color overview_highlight;
    float boot_seconds { 0 };
    float boot_bar_width { 0 };
    float boot_bar_height { 0 };
    float boot_logo { 0 };
    Gfx::Color boot_background;
    Gfx::Color boot_foreground;
    Gfx::Color boot_track;
};

[[nodiscard]] ErrorOr<Theme> load_theme(StringView path);

}
