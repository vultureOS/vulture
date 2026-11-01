/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/HashMap.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Painter.h>
#include <LibText/Font.h>

// How things look: nothing visual is written in pranaUI's code. Colors come from
// /System/Library/Themes/{Light,Dark}.json ("ui"), and sizes, which do not change
// with the appearance, from Metrics.json. Each is a dotted path: "button.fill",
// "button.regular.height". A token that is not there draws magenta, and counts in
// missing_tokens(), which the tests hold at zero.

namespace UI {

enum class ControlSize : u8 {
    Regular,
    Large,
    Small,
    Mini,
};

[[nodiscard]] StringView name_of(ControlSize);

// Hue around the wheel, 0 to 1; the rest also 0 to 1.
struct HSV {
    float hue { 0 };
    float saturation { 0 };
    float value { 1 };
};
[[nodiscard]] HSV hsv_from_color(Gfx::Color);
[[nodiscard]] Gfx::Color color_from_hsv(HSV, u8 alpha = 255);

enum class FontWeight : u8 {
    Regular,
    Medium,
    SemiBold,
    Bold,
};

struct FontSpec {
    float size { 13 };
    FontWeight weight { FontWeight::Regular };
    bool mono { false };

    bool operator==(FontSpec const&) const = default;
};

class Appearance {
public:
    [[nodiscard]] static Appearance& the();
    // Loads "Light" or "Dark" and makes it the current one; whoever asked to hear
    // of it (the windows) is told.
    static ErrorOr<void> use(StringView name);
    [[nodiscard]] static bool is_loaded();

    [[nodiscard]] StringView name() const { return m_name.view(); }
    [[nodiscard]] bool is_dark() const { return m_dark; }

    [[nodiscard]] Gfx::Color color(StringView key) const;
    [[nodiscard]] float metric(StringView key) const;
    [[nodiscard]] Span<float const> list(StringView key) const;
    // "button" Regular "height": "button.regular.height".
    [[nodiscard]] float metric(StringView group, ControlSize, StringView name) const;
    [[nodiscard]] bool has_color(StringView key) const { return m_colors.contains(key); }
    [[nodiscard]] bool has_metric(StringView key) const { return m_metrics.contains(key); }
    [[nodiscard]] bool has_list(StringView key) const { return m_lists.contains(key); }

    // The accent replaced (as System Settings would). Whatever is drawn in the accent
    // (selection, default buttons, switches, ...) moves to its hue, keeping how light or
    // deep each of them was against the theme's own accent.
    void set_accent(Gfx::Color);
    [[nodiscard]] Gfx::Color accent() const { return m_accent; }

    [[nodiscard]] static size_t missing_tokens();

    // Text, at the scale of what it is drawn on.
    [[nodiscard]] static Text::Font& font(FontSpec, float scale);
    // In points, as a 2x display would lay it out: the same at every scale.
    [[nodiscard]] static float text_width(FontSpec, StringView text);
    [[nodiscard]] static float ascent(FontSpec);
    [[nodiscard]] static float descent(FontSpec);
    [[nodiscard]] static float line_height(FontSpec);
    [[nodiscard]] static float cap_height(FontSpec);
    // Named sizes: "body", "small", "headline", ...: fonts.<name>.{size,weight}.
    [[nodiscard]] FontSpec named_font(StringView name) const;
    // The text of a control of a size: fields and plain labels; and what goes on a
    // bezel (buttons, pop-ups, segments), a little heavier.
    [[nodiscard]] FontSpec control_font(ControlSize) const;
    [[nodiscard]] FontSpec bezel_font(ControlSize) const;

private:
    Appearance() = default;
    ErrorOr<void> merge(StringView prefix, void const* value);
    void recolor_for_accent();
    [[nodiscard]] Gfx::Color theme_accent() const;

    String m_name;
    bool m_dark { false };
    Gfx::Color m_accent;
    HashMap<String, Gfx::Color> m_colors;
    // The tokens as the theme has them, for those the accent moves.
    HashMap<String, Gfx::Color> m_theme_colors;
    HashMap<String, float> m_metrics;
    HashMap<String, Vector<float>> m_lists;
};

}
