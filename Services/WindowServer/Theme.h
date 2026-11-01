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
    MaterialStyle materials[static_cast<int>(Window::Material::Count)];
};

[[nodiscard]] ErrorOr<Theme> load_theme(StringView path);

}
