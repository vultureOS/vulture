/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Rasterizer.h>
#include <LibGfx/Stroker.h>
#include <LibTest/TestCase.h>

using namespace Gfx;
namespace Math = Base::Math;

namespace {

double area_of(Path const& path)
{
    double total = 0;
    Rasterizer rasterizer({ -32, -32, 128, 128 });
    rasterizer.add_path(path);
    rasterizer.rasterize(FillRule::NonZero, [&](int, int, int count, u8 const* coverage) {
        for (int i = 0; i < count; ++i)
            total += coverage[i] / 255.0;
    });
    return total;
}

bool near(double a, double b, double tolerance)
{
    return Math::absolute(a - b) <= tolerance;
}

Path line(FloatPoint from, FloatPoint to)
{
    Path path;
    path.move_to(from);
    path.line_to(to);
    return path;
}

}

TEST_CASE(caps)
{
    auto segment = line({ 10, 20 }, { 50, 20 });
    StrokeStyle style;
    style.width = 4;
    EXPECT(near(area_of(stroked(segment, style)), 160, 0.01));
    style.cap = LineCap::Square;
    EXPECT(near(area_of(stroked(segment, style)), 176, 0.01));
    style.cap = LineCap::Round;
    EXPECT(near(area_of(stroked(segment, style)), 160 + Math::pi * 4, 0.3));
}

TEST_CASE(slanted_lines_keep_their_width)
{
    auto segment = line({ 10, 10 }, { 40, 50 }); // 3-4-5: length 50.
    StrokeStyle style;
    style.width = 2;
    EXPECT(near(area_of(stroked(segment, style)), 100, 0.05));
}

TEST_CASE(joins)
{
    Path corner;
    corner.move_to({ 10, 40 });
    corner.line_to({ 40, 40 });
    corner.line_to({ 40, 10 });
    StrokeStyle style;
    style.width = 6;
    // Two arms of 30 x 6, sharing the inner square of 3 x 3, plus the outer
    // corner piece: a square of 3 x 3 mitered, half of it beveled, a quarter
    // circle round.
    double arms = 30 * 6 * 2 - 3 * 3;
    style.join = LineJoin::Miter;
    EXPECT(near(area_of(stroked(corner, style)), arms + 9, 0.1));
    style.join = LineJoin::Bevel;
    EXPECT(near(area_of(stroked(corner, style)), arms + 4.5, 0.1));
    style.join = LineJoin::Round;
    EXPECT(near(area_of(stroked(corner, style)), arms + Math::pi * 9 / 4, 0.2));

    // A sharp corner beyond the miter limit bevels instead.
    Path sharp;
    sharp.move_to({ 10, 30 });
    sharp.line_to({ 50, 32 });
    sharp.line_to({ 10, 34 });
    StrokeStyle limited;
    limited.width = 2;
    limited.miter_limit = 2;
    double beveled = area_of(stroked(sharp, limited));
    limited.miter_limit = 100;
    EXPECT(area_of(stroked(sharp, limited)) > beveled + 1);
}

TEST_CASE(closed_shapes_are_rings)
{
    Path square;
    square.add_rect({ 10, 10, 40, 40 });
    StrokeStyle style;
    style.width = 2;
    // Outer 42 squared less inner 38 squared.
    EXPECT(near(area_of(stroked(square, style)), 42 * 42 - 38 * 38, 0.1));

    Path circle;
    circle.add_circle({ 30, 30 }, 20);
    EXPECT(near(area_of(stroked(circle, style)), Math::pi * (21 * 21 - 19 * 19), 1.0));
}

TEST_CASE(dashes)
{
    auto segment = line({ 0, 10 }, { 40, 10 });
    StrokeStyle style;
    style.width = 2;
    Vector<float> pattern;
    pattern.append(5);
    pattern.append(5);
    style.dashes = pattern;
    EXPECT(near(area_of(stroked(segment, style)), 40, 0.1));
    style.dash_phase = 5;
    EXPECT(near(area_of(stroked(segment, style)), 40, 0.1));
    style.dash_phase = 2.5f;
    EXPECT(near(area_of(stroked(segment, style)), 40, 0.1));
}

TEST_CASE(degenerate_cases)
{
    StrokeStyle style;
    style.width = 4;
    // A point: only what its cap draws.
    auto dot = line({ 20, 20 }, { 20, 20 });
    EXPECT(near(area_of(stroked(dot, style)), 0, 0));
    style.cap = LineCap::Square;
    EXPECT(near(area_of(stroked(dot, style)), 16, 0.01));
    style.cap = LineCap::Round;
    EXPECT(near(area_of(stroked(dot, style)), Math::pi * 4, 0.2));
    // No width, no stroke; a line doubling back still has its width.
    style.width = 0;
    EXPECT(stroked(line({ 0, 0 }, { 9, 9 }), style).is_empty());
    Path back;
    back.move_to({ 10, 10 });
    back.line_to({ 40, 10 });
    back.line_to({ 20, 10 });
    StrokeStyle thin;
    thin.width = 2;
    EXPECT(near(area_of(stroked(back, thin)), 60, 12));
}
