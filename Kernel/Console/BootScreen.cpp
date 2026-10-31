/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Console/BootScreen.h>
#include <LibBase/StdLibExtras.h>

// Shapes are anti-aliased by testing a 4x4 grid of sample points per pixel.
// Geometry is in units of 1/8 pixel and in integers only: the kernel does
// not use the floating-point unit.

namespace Kernel::BootScreen {

namespace {

constexpr Color background_color { 0x00, 0x00, 0x00 };
constexpr Color logo_color { 0xff, 0xff, 0xff };
constexpr Color track_color { 0x3a, 0x3a, 0x3c };
constexpr Color fill_color { 0xff, 0xff, 0xff };

constexpr i64 subpixels = 8;
constexpr i64 samples_per_axis = 4;

struct Point {
    i64 x;
    i64 y;
};

struct Layout {
    i32 logo_x;
    i32 logo_y;
    i32 logo_size;
    i32 bar_x;
    i32 bar_y;
    i32 bar_width;
    i32 bar_height;
};

Layout layout_for(Framebuffer const& framebuffer)
{
    auto width = static_cast<i32>(framebuffer.width());
    auto height = static_cast<i32>(framebuffer.height());
    Layout layout {};
    layout.logo_size = min(width, height) / 6;
    layout.logo_x = (width - layout.logo_size) / 2;
    layout.logo_y = height * 2 / 5 - layout.logo_size / 2;
    layout.bar_width = layout.logo_size * 5 / 4;
    layout.bar_height = max(4, layout.logo_size / 24);
    layout.bar_x = (width - layout.bar_width) / 2;
    layout.bar_y = layout.logo_y + layout.logo_size + layout.logo_size / 2;
    return layout;
}

i64 squared_length(Point a, Point b)
{
    i64 dx = a.x - b.x;
    i64 dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Whether `point` lies within `radius` of the segment from `from` to `to`
// (a capsule), without dividing: the perpendicular case compares
// cross^2 <= radius^2 * |segment|^2.
bool within_segment(Point point, Point from, Point to, i64 radius)
{
    Point segment { to.x - from.x, to.y - from.y };
    Point offset { point.x - from.x, point.y - from.y };
    i64 projection = offset.x * segment.x + offset.y * segment.y;
    i64 length_squared = segment.x * segment.x + segment.y * segment.y;
    if (projection <= 0)
        return squared_length(point, from) <= radius * radius;
    if (projection >= length_squared)
        return squared_length(point, to) <= radius * radius;
    i64 cross = offset.x * segment.y - offset.y * segment.x;
    return cross * cross <= radius * radius * length_squared;
}

// Fills the pixels in [x, x + width) x [y, y + height) with `color`,
// weighted by how many sample points `inside` accepts.
template<typename Inside>
void draw_shape(Framebuffer& framebuffer, i32 x, i32 y, i32 width, i32 height, Color color, Inside inside)
{
    for (i32 row = y; row < y + height; ++row) {
        for (i32 column = x; column < x + width; ++column) {
            u32 hits = 0;
            for (i64 sample_y = 0; sample_y < samples_per_axis; ++sample_y) {
                for (i64 sample_x = 0; sample_x < samples_per_axis; ++sample_x) {
                    Point point { column * subpixels + sample_x * 2 + 1, row * subpixels + sample_y * 2 + 1 };
                    if (inside(point))
                        ++hits;
                }
            }
            framebuffer.blend_pixel(column, row, color, hits * 256 / (samples_per_axis * samples_per_axis));
        }
    }
}

// The logo: a ring around a bold "V". Coordinates are thousandths of the
// logo size, measured from its center.
void draw_logo(Framebuffer& framebuffer, Layout const& layout)
{
    i64 size = layout.logo_size * subpixels;
    Point center { layout.logo_x * subpixels + size / 2, layout.logo_y * subpixels + size / 2 };
    auto at = [&](i64 x, i64 y) { return Point { center.x + x * size / 1000, center.y + y * size / 1000 }; };

    i64 outer_radius = size / 2;
    i64 inner_radius = outer_radius - size * 70 / 1000;
    Point left = at(-290, -170);
    Point bottom = at(0, 260);
    Point right = at(290, -170);
    i64 stroke_radius = size * 55 / 1000;

    draw_shape(framebuffer, layout.logo_x, layout.logo_y, layout.logo_size, layout.logo_size, logo_color, [&](Point point) {
        i64 distance_squared = squared_length(point, center);
        bool in_ring = distance_squared <= outer_radius * outer_radius && distance_squared >= inner_radius * inner_radius;
        return in_ring || within_segment(point, left, bottom, stroke_radius) || within_segment(point, bottom, right, stroke_radius);
    });
}

// A pill shape: a capsule around the horizontal center line.
void draw_pill(Framebuffer& framebuffer, i32 x, i32 y, i32 width, i32 height, Color color)
{
    i64 radius = height * subpixels / 2;
    i64 center_y = y * subpixels + radius;
    Point from { x * subpixels + radius, center_y };
    Point to { (x + width) * subpixels - radius, center_y };
    draw_shape(framebuffer, x, y, width, height, color, [&](Point point) { return within_segment(point, from, to, radius); });
}

}

void show(Framebuffer& framebuffer)
{
    framebuffer.fill_rect(0, 0, static_cast<i32>(framebuffer.width()), static_cast<i32>(framebuffer.height()), background_color);
    auto layout = layout_for(framebuffer);
    draw_logo(framebuffer, layout);
    set_progress(framebuffer, 0);
}

void set_progress(Framebuffer& framebuffer, u32 percent)
{
    auto layout = layout_for(framebuffer);
    percent = min(percent, 100u);
    framebuffer.fill_rect(layout.bar_x, layout.bar_y, layout.bar_width, layout.bar_height, background_color);
    draw_pill(framebuffer, layout.bar_x, layout.bar_y, layout.bar_width, layout.bar_height, track_color);
    if (percent == 0)
        return;
    i32 filled = max(layout.bar_height, static_cast<i32>(layout.bar_width * static_cast<i32>(percent) / 100));
    draw_pill(framebuffer, layout.bar_x, layout.bar_y, filled, layout.bar_height, fill_color);
}

}
