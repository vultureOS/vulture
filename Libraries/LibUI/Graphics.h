/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/StringView.h>
#include <LibGfx/Painter.h>
#include <LibUI/Appearance.h>

// What views draw with: all in points, on the scale of the window underneath.

namespace UI {

enum class TextAlignment : u8 {
    Left,
    Center,
    Right,
};

enum class Symbol : u8 {
    Checkmark,
    Dash,
    ChevronUpDown,
    ChevronDown,
    ChevronUp,
    ChevronRight,
    ChevronLeft,
    Disclosure,
    Plus,
    Minus,
    Magnifier,
    ClearCircle,
    Question,
    Folder,
    File,
    Sidebar,
    Home,
    Desktop,
    Download,
    Applications,
    Trash,
    Drive,
    Refresh,
    Speaker,
    Globe,
    Lock,
    Eject,
    FirstAid,
    Erase,
    Mount,
    Partition,
    Contrast,
    Photo,
    Keyboard,
    Bell,
    Person,
    Information,
    Accessibility,
    Display,
    Mouse,
    Trackpad,
    // Two switches, one on: Control Center's.
    Toggles,
    // A crescent: Focus, and Do Not Disturb.
    Moon,
};

// A control's face: a shape with shadows outside it, a fill, shading inside it.
// The groups are in the theme: <group>.fill, or .fillTop and .fillBottom; .outer and
// .inner (lists of top, bottom, side, alpha in fours: rings of black reaching that
// far out from, or in from, the edges of the shape; .outerColor to tint those outside);
// and .soft (how much of the
// shape's edge, at 2x and up, spreads one pixel beyond it).
struct BezelPaint {
    Gfx::Color top;
    Gfx::Color bottom;
    Span<float const> outer;
    Span<float const> inner;
    // As `inner`, of white.
    Span<float const> inner_white;
    Gfx::Color outer_color { 0, 0, 0, 255 };
    Gfx::Color highlight;
    float soft { 0 };

    [[nodiscard]] static BezelPaint from_tokens(StringView group);
};

class Graphics {
public:
    Graphics(Gfx::Painter&, float scale);

    [[nodiscard]] Gfx::Painter& painter() { return m_painter; }
    [[nodiscard]] float scale() const { return m_scale; }

    void save() { m_painter.save(); }
    void restore() { m_painter.restore(); }
    void translate(float x, float y) { m_painter.translate(x, y); }
    void clip(Gfx::FloatRect const& rect) { m_painter.clip_rect(rect); }

    // To the nearest device pixel, for lines and edges that should stay crisp,
    // wherever the view is.
    [[nodiscard]] float snap(float position) const;
    [[nodiscard]] Gfx::FloatRect snapped(Gfx::FloatRect const&) const;
    // One device pixel, in points.
    [[nodiscard]] float pixel() const { return 1 / m_scale; }

    void fill(Gfx::FloatRect const&, Gfx::Color);
    void fill_rounded(Gfx::FloatRect const&, float radius, Gfx::Color);
    void fill_continuous(Gfx::FloatRect const&, float radius, Gfx::Color);
    void fill_vertical_gradient(Gfx::FloatRect const&, float radius, Gfx::Color top, Gfx::Color bottom);
    void fill_circle(Gfx::FloatPoint center, float radius, Gfx::Color);
    // A ring just inside this shape.
    void stroke_rounded(Gfx::FloatRect const&, float radius, float width, Gfx::Color);
    void line(Gfx::FloatPoint from, Gfx::FloatPoint to, float width, Gfx::Color, bool round_caps = false);

    // The baseline's left end at `position` (its height goes to the pixel grid).
    void draw_text(StringView, FontSpec, Gfx::FloatPoint position, Gfx::Color);
    // Vertically centered by cap height, as control labels are.
    void draw_text_centered(StringView, FontSpec, Gfx::FloatRect const&, Gfx::Color, TextAlignment = TextAlignment::Center);
    // As much as fits, with an ellipsis if not all does.
    void draw_text_truncated(StringView, FontSpec, Gfx::FloatPoint position, float max_width, Gfx::Color);

    void draw_bezel(Gfx::FloatRect const&, float radius, BezelPaint const&);
    void draw_symbol(Symbol, Gfx::FloatRect const&, Gfx::Color, float weight = 0);

private:
    Gfx::Painter& m_painter;
    float m_scale;
};

}
