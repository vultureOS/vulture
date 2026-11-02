/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Cursors.h>
#include <LibTest/TestCase.h>

using namespace Gfx;
using Window::CursorShape;

namespace {

constexpr int shape_count = static_cast<int>(CursorShape::Count);

u32 alpha_of(Pixel pixel) { return pixel >> 24; }
u32 red_of(Pixel pixel) { return (pixel >> 16) & 0xff; }
u32 green_of(Pixel pixel) { return (pixel >> 8) & 0xff; }
u32 blue_of(Pixel pixel) { return pixel & 0xff; }

bool is_opaque_gray(Pixel pixel, u32 level)
{
    return alpha_of(pixel) == 255 && red_of(pixel) == level && green_of(pixel) == level && blue_of(pixel) == level;
}

}

TEST_CASE(every_shape_has_room_for_its_shadow_and_its_hot_spot_is_in_the_picture)
{
    for (int scale = 1; scale <= 2; ++scale) {
        for (int shape = 0; shape < shape_count; ++shape) {
            auto const cursor = WindowServer::make_cursor(static_cast<CursorShape>(shape), scale);
            int const size = cursor.image->width();
            EXPECT(cursor.hot_spot.x >= 0 && cursor.hot_spot.x < size);
            EXPECT(cursor.hot_spot.y >= 0 && cursor.hot_spot.y < size);
            // Nothing cut off: the picture's edges are clear.
            for (int i = 0; i < size; ++i) {
                EXPECT_EQ(alpha_of(cursor.image->pixel_at(i, 0)), 0u);
                EXPECT_EQ(alpha_of(cursor.image->pixel_at(0, i)), 0u);
                EXPECT_EQ(alpha_of(cursor.image->pixel_at(i, cursor.image->height() - 1)), 0u);
                EXPECT_EQ(alpha_of(cursor.image->pixel_at(size - 1, i)), 0u);
            }
        }
    }
}

TEST_CASE(pixels_are_premultiplied_as_the_driver_expects)
{
    for (int scale = 1; scale <= 2; ++scale) {
        for (int shape = 0; shape < shape_count; ++shape) {
            auto const cursor = WindowServer::make_cursor(static_cast<CursorShape>(shape), scale);
            for (int y = 0; y < cursor.image->height(); ++y) {
                for (int x = 0; x < cursor.image->width(); ++x) {
                    auto const pixel = cursor.image->pixel_at(x, y);
                    EXPECT(red_of(pixel) <= alpha_of(pixel) && green_of(pixel) <= alpha_of(pixel) && blue_of(pixel) <= alpha_of(pixel));
                }
            }
        }
    }
}

TEST_CASE(the_arrow_is_sharp)
{
    for (int scale = 1; scale <= 2; ++scale) {
        auto const cursor = WindowServer::make_cursor(CursorShape::Arrow, scale);
        auto const& image = *cursor.image;
        int const left = cursor.hot_spot.x;
        int const top = cursor.hot_spot.y;
        // Partway down its straight left side: black from exactly the hot spot's column, and a point of white
        // before it, with nothing in between.
        int const y = top + 8 * scale;
        EXPECT(is_opaque_gray(image.pixel_at(left, y), 0));
        for (int i = 1; i <= scale; ++i)
            EXPECT(is_opaque_gray(image.pixel_at(left - i, y), 255));
        // And along the flat bottom of its wing, likewise.
        int const x = left + 8 * scale;
        int const wing_bottom = top + 11 * scale;
        EXPECT(is_opaque_gray(image.pixel_at(x, wing_bottom - 1), 0));
        for (int i = 0; i < scale; ++i)
            EXPECT(is_opaque_gray(image.pixel_at(x, wing_bottom + i), 255));
        // The hot spot is at its tip: what's above and to the left of that isn't black.
        EXPECT(alpha_of(image.pixel_at(left - scale - 1, top - scale - 1)) < 255);
    }
}

TEST_CASE(a_two_times_arrow_is_the_same_arrow)
{
    auto const one = WindowServer::make_cursor(CursorShape::Arrow, 1);
    auto const two = WindowServer::make_cursor(CursorShape::Arrow, 2);
    EXPECT_EQ(two.hot_spot.x, one.hot_spot.x * 2);
    EXPECT_EQ(two.hot_spot.y, one.hot_spot.y * 2);
    int differences = 0;
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 24; ++x) {
            // Only where it's wholly one thing at 1x: slanted edges are meant to differ.
            auto const small = one.image->pixel_at(x, y);
            if (!is_opaque_gray(small, 0) && !is_opaque_gray(small, 255))
                continue;
            if (two.image->pixel_at(x * 2, y * 2) != small)
                ++differences;
        }
    }
    EXPECT_EQ(differences, 0);
}
