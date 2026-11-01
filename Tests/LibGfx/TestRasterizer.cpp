/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/Vector.h>
#include <LibGfx/Rasterizer.h>
#include <LibTest/TestCase.h>

using namespace Gfx;
namespace Math = Base::Math;

namespace {

// Coverage of a path as a grid of 0..255.
struct Grid {
    int width;
    int height;
    Vector<u8> cells;

    [[nodiscard]] u8 at(int x, int y) const { return cells[static_cast<size_t>(y * width + x)]; }

    // Total covered area, in pixels.
    [[nodiscard]] double area() const
    {
        double total = 0;
        for (u8 cell : cells)
            total += cell / 255.0;
        return total;
    }
};

Grid raster(Path const& path, int width, int height, FillRule rule = FillRule::NonZero, IntRect* clip_override = nullptr)
{
    Grid grid { width, height, {} };
    MUST(grid.cells.try_resize(static_cast<size_t>(width * height)));
    __builtin_memset(grid.cells.data(), 0, grid.cells.size());
    IntRect clip = clip_override ? *clip_override : IntRect { 0, 0, width, height };
    Rasterizer rasterizer(clip);
    rasterizer.add_path(path);
    rasterizer.rasterize(rule, [&](int y, int x, int count, u8 const* coverage) {
        for (int i = 0; i < count; ++i) {
            EXPECT(x + i >= clip.x && x + i < clip.right() && y >= clip.y && y < clip.bottom());
            grid.cells[static_cast<size_t>(y * width + x + i)] = coverage[i];
        }
    });
    return grid;
}

bool near(double a, double b, double tolerance)
{
    return Math::absolute(a - b) <= tolerance;
}

}

TEST_CASE(whole_pixels)
{
    Path path;
    path.add_rect({ 1, 1, 2, 2 });
    auto grid = raster(path, 4, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
            EXPECT_EQ(grid.at(x, y), inside ? 255 : 0);
        }
    }
}

TEST_CASE(fractions_of_pixels)
{
    Path half;
    half.add_rect({ 1.5f, 1, 1, 1 });
    auto grid = raster(half, 4, 3);
    EXPECT_EQ(grid.at(0, 1), 0);
    EXPECT(near(grid.at(1, 1), 127.5, 1));
    EXPECT(near(grid.at(2, 1), 127.5, 1));
    EXPECT_EQ(grid.at(3, 1), 0);

    Path quarter;
    quarter.add_rect({ 0.5f, 0.5f, 1, 1 });
    auto corners = raster(quarter, 2, 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x)
            EXPECT(near(corners.at(x, y), 63.75, 1));
    }
}

TEST_CASE(area_is_exact)
{
    // A triangle: half base times height, whatever the angles.
    Path triangle;
    triangle.move_to({ 3.3f, 2.1f });
    triangle.line_to({ 57.7f, 9.4f });
    triangle.line_to({ 21.2f, 48.9f });
    triangle.close();
    double twice = Math::absolute((57.7 - 3.3) * (48.9 - 2.1) - (21.2 - 3.3) * (9.4 - 2.1));
    EXPECT(near(raster(triangle, 64, 64).area(), twice / 2, 0.15));

    // A circle, through its Bézier arcs.
    Path circle;
    circle.add_circle({ 32, 32 }, 25);
    EXPECT(near(raster(circle, 64, 64).area(), Math::pi * 625, 2.5));

    Path arc;
    arc.arc_to({ 32, 32 }, 25, 0, static_cast<float>(2 * Math::pi));
    arc.close();
    EXPECT(near(raster(arc, 64, 64).area(), Math::pi * 625, 2.5));

    // A nearly flat sliver crosses many pixels in a row.
    Path sliver;
    sliver.move_to({ 1, 10 });
    sliver.line_to({ 63, 11 });
    sliver.line_to({ 63, 12 });
    sliver.line_to({ 1, 11 });
    sliver.close();
    EXPECT(near(raster(sliver, 64, 20).area(), 62, 0.1));
}

TEST_CASE(direction_does_not_matter)
{
    Path clockwise, counter;
    clockwise.move_to({ 2, 2 });
    clockwise.line_to({ 8, 2 });
    clockwise.line_to({ 8, 8 });
    clockwise.close();
    counter.move_to({ 2, 2 });
    counter.line_to({ 8, 8 });
    counter.line_to({ 8, 2 });
    counter.close();
    auto a = raster(clockwise, 10, 10);
    auto b = raster(counter, 10, 10);
    for (size_t i = 0; i < a.cells.size(); ++i)
        EXPECT_EQ(a.cells[i], b.cells[i]);
}

TEST_CASE(fill_rules)
{
    // Two nested squares the same way around: a solid block, or a frame.
    Path path;
    path.add_rect({ 0, 0, 10, 10 });
    path.add_rect({ 3, 3, 4, 4 });
    auto non_zero = raster(path, 10, 10, FillRule::NonZero);
    auto even_odd = raster(path, 10, 10, FillRule::EvenOdd);
    EXPECT_EQ(non_zero.at(5, 5), 255);
    EXPECT_EQ(even_odd.at(5, 5), 0);
    EXPECT_EQ(non_zero.at(1, 1), 255);
    EXPECT_EQ(even_odd.at(1, 1), 255);

    // The inner one the other way around: a hole under either rule.
    Path holed;
    holed.add_rect({ 0, 0, 10, 10 });
    holed.move_to({ 3, 3 });
    holed.line_to({ 3, 7 });
    holed.line_to({ 7, 7 });
    holed.line_to({ 7, 3 });
    holed.close();
    EXPECT_EQ(raster(holed, 10, 10, FillRule::NonZero).at(5, 5), 0);
    EXPECT(near(raster(holed, 10, 10).area(), 84, 0.01));
}

TEST_CASE(clipping)
{
    Path path;
    path.add_rect({ -100, -100, 300, 300 });
    IntRect clip { 2, 3, 4, 5 };
    auto grid = raster(path, 10, 10, FillRule::NonZero, &clip);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x)
            EXPECT_EQ(grid.at(x, y), clip.contains(IntPoint { x, y }) ? 255 : 0);
    }

    // Leaning across the clip's sides: what is inside is the same as without.
    Path diagonal;
    diagonal.move_to({ -30, 0 });
    diagonal.line_to({ 40, 0 });
    diagonal.line_to({ 20, 40 });
    diagonal.line_to({ -10, 40 });
    diagonal.close();
    auto whole = raster(diagonal, 40, 40);
    IntRect window { 5, 5, 10, 10 };
    auto part = raster(diagonal, 40, 40, FillRule::NonZero, &window);
    for (int y = window.y; y < window.bottom(); ++y) {
        for (int x = window.x; x < window.right(); ++x)
            EXPECT(near(part.at(x, y), whole.at(x, y), 1));
    }

    // Wholly outside, or crossing rows nobody sees.
    Path outside;
    outside.add_rect({ 50, 50, 10, 10 });
    EXPECT(near(raster(outside, 10, 10).area(), 0, 0));
    Path right_of_clip;
    right_of_clip.add_rect({ 20, 1, 5, 5 });
    EXPECT(near(raster(right_of_clip, 10, 10).area(), 0, 0));
}

TEST_CASE(open_subpaths_close_themselves)
{
    Path open;
    open.move_to({ 1, 1 });
    open.line_to({ 5, 1 });
    open.line_to({ 5, 5 });
    open.line_to({ 1, 5 });
    open.move_to({ 7, 1 });
    open.line_to({ 9, 1 });
    open.line_to({ 9, 3 });
    open.line_to({ 7, 3 });
    EXPECT(near(raster(open, 10, 10).area(), 20, 0.01));
}

TEST_CASE(rounded_rectangles)
{
    Path rounded;
    rounded.add_rounded_rect({ 4, 4, 40, 30 }, 8);
    // Area: the rectangle less four corners of (4 - pi) r^2 / 4.
    double expected = 40 * 30 - (4 - Math::pi) * 64;
    auto grid = raster(rounded, 48, 38);
    EXPECT(near(grid.area(), expected, 0.8));
    EXPECT_EQ(grid.at(4, 4), 0);
    EXPECT_EQ(grid.at(24, 19), 255);
    // Symmetric, left and right, top and bottom.
    for (int y = 0; y < 38; ++y) {
        for (int x = 0; x < 48; ++x) {
            EXPECT(near(grid.at(x, y), grid.at(47 - x, y), 2));
            EXPECT(near(grid.at(x, y), grid.at(x, 37 - y), 2));
        }
    }

    // A continuous corner cuts more of the corner off than a plain one of
    // the same radius, near the corner, and meets the edge farther along.
    Path continuous;
    continuous.add_continuous_rounded_rect({ 4, 4, 40, 30 }, 8);
    auto smooth = raster(continuous, 48, 38);
    EXPECT(smooth.area() < grid.area());
    EXPECT(smooth.area() > grid.area() - 16);
    EXPECT_EQ(smooth.at(24, 19), 255);
    for (int y = 0; y < 38; ++y) {
        for (int x = 0; x < 48; ++x)
            EXPECT(near(smooth.at(x, y), smooth.at(47 - x, y), 2));
    }
    EXPECT(smooth.at(12, 4) < 255);
    EXPECT_EQ(grid.at(12, 4), 255);

    // A radius too big for the box settles for what fits.
    Path capsule;
    capsule.add_rounded_rect({ 0, 0, 20, 10 }, 100);
    EXPECT(near(raster(capsule, 20, 10).area(), 100 + Math::pi * 25 - 100 + 0, 200));
    Path big_continuous;
    big_continuous.add_continuous_rounded_rect({ 0, 0, 20, 20 }, 100);
    EXPECT_EQ(raster(big_continuous, 20, 20).at(10, 10), 255);
}
