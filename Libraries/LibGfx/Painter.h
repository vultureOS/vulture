/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/RefPtr.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Gradient.h>
#include <LibGfx/Path.h>
#include <LibGfx/Rasterizer.h>
#include <LibGfx/Stroker.h>

namespace Gfx {

// Draws on a bitmap, Quartz-style: a current transform and clip, saved and
// restored as a stack. Everything is antialiased by exact coverage.
class Painter {
public:
    explicit Painter(Bitmap&);

    void save();
    void restore();

    void translate(float x, float y);
    void scale(float x, float y);
    void rotate(float radians);
    void concat(AffineTransform const&);
    [[nodiscard]] AffineTransform const& transform() const { return m_state.transform; }
    void set_transform(AffineTransform const& transform) { m_state.transform = transform; }

    // Clips only shrink, until restore().
    void clip_rect(FloatRect const&);
    void clip_path(Path const&, FillRule = FillRule::NonZero);
    [[nodiscard]] IntRect clip_bounds() const { return m_state.clip; }

    void fill_rect(FloatRect const&, Color);
    void fill_path(Path const&, Color, FillRule = FillRule::NonZero);
    void fill_path(Path const&, Gradient const&, FillRule = FillRule::NonZero);
    void stroke_path(Path const&, StrokeStyle const&, Color);
    void stroke_path(Path const&, StrokeStyle const&, Gradient const&);
    void fill_rounded_rect(FloatRect const&, float radius, Color);
    void fill_continuous_rounded_rect(FloatRect const&, float radius, Color);

    // Bilinear, and with averaging when shrinking by more than half.
    void draw_bitmap(Bitmap const&, FloatRect const& destination, float opacity = 1);
    void draw_bitmap(Bitmap const&, FloatRect const& source, FloatRect const& destination, float opacity = 1);

    // Pixel for pixel, in device coordinates (no transform), within the clip.
    void blit(Bitmap const&, IntPoint position, IntRect source, u32 opacity = 255);
    // A coverage mask (a glyph, a shadow) filled with a color, likewise.
    void draw_mask(AlphaMask const&, IntPoint position, Color);
    void draw_mask(u8 const* values, int pitch, IntRect destination, Color);

private:
    struct ClipMask : RefCounted<ClipMask> {
        IntRect area;
        Vector<u8> coverage;
        [[nodiscard]] u8 const* row(int y) const { return coverage.data() + static_cast<size_t>(y - area.y) * static_cast<size_t>(area.width); }
    };

    struct State {
        AffineTransform transform;
        IntRect clip;
        RefPtr<ClipMask> mask;
    };

    // Fills pixels from `x` for `count`, on row `y`.
    using Shader = void (*)(void* context, int y, int x, int count, Pixel* row);

    void fill_device_path(Path const&, FillRule, Pixel solid, Shader, void* context);
    void fill_with_gradient(Path const& device_path, Gradient const&, FillRule);
    [[nodiscard]] u8 const* masked_coverage(int y, int x, int count, u8 const* coverage);

    Bitmap& m_target;
    State m_state;
    Vector<State> m_stack;
    Vector<Pixel> m_shader_row;
    Vector<u8> m_coverage_row;
};

}
