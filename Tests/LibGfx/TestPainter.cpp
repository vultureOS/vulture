/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Blend.h>
#include <LibGfx/Painter.h>
#include <LibTest/TestCase.h>

using namespace Gfx;
namespace Math = Base::Math;

namespace {

NonnullRefPtr<Bitmap> white_bitmap(int width, int height)
{
    auto bitmap = MUST(Bitmap::create({ width, height }));
    bitmap->clear(Color::from_rgb(0xffffff));
    return bitmap;
}

bool near(u8 a, u8 b, u8 tolerance = 1)
{
    int difference = static_cast<int>(a) - static_cast<int>(b);
    return difference <= tolerance && difference >= -static_cast<int>(tolerance);
}

constexpr Color black { 0, 0, 0, 255 };
constexpr Color red { 255, 0, 0, 255 };

}

TEST_CASE(fill_rect_whole_and_fractional)
{
    auto bitmap = white_bitmap(8, 8);
    Painter painter(*bitmap);
    painter.fill_rect({ 1, 1, 2, 2 }, black);
    EXPECT(bitmap->color_at(1, 1) == black);
    EXPECT(bitmap->color_at(3, 1) == Color::from_rgb(0xffffff));

    painter.fill_rect({ 4.5f, 4, 1, 1 }, black);
    EXPECT(near(bitmap->color_at(4, 4).red, 128));
    EXPECT(near(bitmap->color_at(5, 4).red, 128));

    // Translucent over white.
    painter.fill_rect({ 6, 6, 1, 1 }, black.with_alpha(128));
    EXPECT(near(bitmap->color_at(6, 6).red, 127));
}

TEST_CASE(transforms_apply_to_what_is_drawn)
{
    auto bitmap = white_bitmap(16, 16);
    Painter painter(*bitmap);
    painter.save();
    painter.translate(4, 4);
    painter.scale(2, 2);
    painter.fill_rect({ 0, 0, 2, 2 }, black);
    painter.restore();
    EXPECT(bitmap->color_at(4, 4) == black);
    EXPECT(bitmap->color_at(7, 7) == black);
    EXPECT(bitmap->color_at(8, 8).red == 255);
    EXPECT(bitmap->color_at(3, 3).red == 255);
    // After restore, the plain transform again.
    painter.fill_rect({ 0, 0, 1, 1 }, black);
    EXPECT(bitmap->color_at(0, 0) == black);

    // Rotated a quarter turn about the origin: (x, y) -> (-y, x).
    auto turned = white_bitmap(16, 16);
    Painter turning(*turned);
    turning.translate(8, 8);
    turning.rotate(static_cast<float>(Math::pi / 2));
    turning.fill_rect({ 2, 0, 4, 2 }, black);
    EXPECT(near(turned->color_at(7, 11).red, 0));
    EXPECT(near(turned->color_at(6, 11).red, 0));
    EXPECT(near(turned->color_at(8, 11).red, 255));
}

TEST_CASE(clipping_by_rect_and_path)
{
    auto bitmap = white_bitmap(16, 16);
    Painter painter(*bitmap);
    painter.save();
    painter.clip_rect({ 4, 4, 4, 4 });
    EXPECT(painter.clip_bounds() == (IntRect { 4, 4, 4, 4 }));
    painter.fill_rect({ 0, 0, 16, 16 }, black);
    painter.restore();
    EXPECT(bitmap->color_at(4, 4) == black);
    EXPECT(bitmap->color_at(3, 4).red == 255);
    EXPECT(bitmap->color_at(8, 4).red == 255);

    auto circled = white_bitmap(32, 32);
    Painter clipped(*circled);
    Path circle;
    circle.add_circle({ 16, 16 }, 10);
    clipped.clip_path(circle);
    clipped.fill_rect({ 0, 0, 32, 32 }, black);
    EXPECT(circled->color_at(16, 16) == black);
    EXPECT(circled->color_at(16, 6).red < 128);
    EXPECT(circled->color_at(16, 5).red == 255);
    EXPECT(circled->color_at(2, 2).red == 255);

    // Clips narrow one another, even a path by a path.
    clipped.save();
    Path half;
    half.add_rect({ 16, 0, 16, 32 });
    clipped.clip_path(half);
    auto over = white_bitmap(32, 32);
    (void)over;
    clipped.fill_rect({ 0, 0, 32, 32 }, red);
    clipped.restore();
    EXPECT(circled->color_at(20, 16) == red);
    EXPECT(circled->color_at(12, 16) == black);
    EXPECT(circled->color_at(30, 16).red == 255 && circled->color_at(30, 16).green == 255);
}

TEST_CASE(gradients)
{
    auto bitmap = white_bitmap(101, 4);
    Painter painter(*bitmap);
    GradientStop const stops[] = { { 0, black }, { 1, Color::from_rgb(0xffffff) } };
    auto gradient = Gradient::linear({ 0, 0 }, { 100, 0 }, { stops, 2 });
    Path area;
    area.add_rect({ 0, 0, 101, 4 });
    painter.fill_path(area, gradient);
    EXPECT(near(bitmap->color_at(0, 0).red, 1, 2));
    EXPECT(near(bitmap->color_at(50, 0).red, 128, 2));
    EXPECT(near(bitmap->color_at(100, 0).red, 255, 2));
    // Monotonic, and the same on every row.
    for (int x = 1; x <= 100; ++x) {
        EXPECT(bitmap->color_at(x, 0).red >= bitmap->color_at(x - 1, 0).red);
        EXPECT(bitmap->color_at(x, 3).red == bitmap->color_at(x, 0).red);
    }

    // Through a transform: the gradient moves with it.
    auto moved = white_bitmap(101, 4);
    Painter moving(*moved);
    moving.translate(50, 0);
    Path shifted;
    shifted.add_rect({ -50, 0, 101, 4 });
    moving.fill_path(shifted, gradient);
    EXPECT(moved->color_at(50, 0).red <= 2);
    EXPECT(moved->color_at(100, 0).red >= 126 && moved->color_at(100, 0).red <= 130);

    // Radial: dark in the middle, light out at the radius.
    auto round = white_bitmap(41, 41);
    Painter radial(*round);
    auto circle = Gradient::radial({ 20.5f, 20.5f }, 20, { stops, 2 });
    Path box;
    box.add_rect({ 0, 0, 41, 41 });
    radial.fill_path(box, circle);
    EXPECT(round->color_at(20, 20).red <= 2);
    EXPECT(near(round->color_at(30, 20).red, 128, 3));
    EXPECT(round->color_at(0, 0).red == 255);
    EXPECT(near(round->color_at(10, 20).red, round->color_at(30, 20).red, 1));

    // Colors premultiply: a fade to clear stays the same color.
    GradientStop const fade[] = { { 0, red }, { 1, red.with_alpha(0) } };
    auto fading = Gradient::linear({ 0, 0 }, { 100, 0 }, { fade, 2 });
    auto over = white_bitmap(101, 1);
    Painter fader(*over);
    Path strip;
    strip.add_rect({ 0, 0, 101, 1 });
    fader.fill_path(strip, fading);
    auto middle = over->color_at(50, 0);
    EXPECT(near(middle.red, 255, 1));
    EXPECT(near(middle.green, 128, 3));
}

TEST_CASE(strokes)
{
    auto bitmap = white_bitmap(20, 10);
    Painter painter(*bitmap);
    Path line;
    line.move_to({ 2, 5 });
    line.line_to({ 18, 5 });
    StrokeStyle style;
    style.width = 2;
    painter.stroke_path(line, style, black);
    EXPECT(bitmap->color_at(10, 4) == black);
    EXPECT(bitmap->color_at(10, 5) == black);
    EXPECT(bitmap->color_at(10, 3).red == 255);
    EXPECT(bitmap->color_at(10, 6).red == 255);

    // Scaled: the width scales too.
    auto scaled = white_bitmap(40, 20);
    Painter scaling(*scaled);
    scaling.scale(2, 2);
    scaling.stroke_path(line, style, black);
    EXPECT(scaled->color_at(20, 8) == black);
    EXPECT(scaled->color_at(20, 11) == black);
    EXPECT(scaled->color_at(20, 7).red == 255);
}

TEST_CASE(bitmaps)
{
    auto source = MUST(Bitmap::create({ 4, 4 }));
    source->clear(red);
    source->set_pixel_at(0, 0, 0);

    // Pixel for pixel, with the clip and transform's move.
    auto bitmap = white_bitmap(8, 8);
    Painter painter(*bitmap);
    painter.draw_bitmap(*source, FloatRect { 2, 2, 4, 4 });
    EXPECT(bitmap->color_at(2, 2).green == 255);
    EXPECT(bitmap->color_at(3, 2) == red);
    EXPECT(bitmap->color_at(5, 5) == red);
    EXPECT(bitmap->color_at(6, 6).green == 255);

    // At half opacity over white.
    auto faded = white_bitmap(8, 8);
    Painter fading(*faded);
    fading.draw_bitmap(*source, FloatRect { 2, 2, 4, 4 }, 0.5f);
    EXPECT(near(faded->color_at(3, 3).green, 127));

    // Doubled: bilinear between the pixels, solid in the middle.
    auto doubled = white_bitmap(16, 16);
    Painter doubling(*doubled);
    doubling.draw_bitmap(*source, FloatRect { 0, 0, 8, 8 });
    EXPECT(doubled->color_at(5, 5) == red);
    EXPECT(doubled->color_at(0, 0).green > 200);

    // Far smaller: every source pixel counts, none is skipped.
    auto checker = MUST(Bitmap::create({ 16, 16 }));
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x)
            checker->set_pixel_at(x, y, ((x + y) & 1) ? black.to_pixel() : Color::from_rgb(0xffffff).to_pixel());
    }
    auto small = white_bitmap(4, 4);
    Painter shrinking(*small);
    shrinking.draw_bitmap(*checker, FloatRect { 0, 0, 4, 4 });
    EXPECT(near(small->color_at(1, 1).red, 128, 12));

    // Through a part of the source.
    auto parted = white_bitmap(4, 4);
    Painter parting(*parted);
    parting.draw_bitmap(*source, FloatRect { 1, 1, 2, 2 }, FloatRect { 0, 0, 2, 2 });
    EXPECT(parted->color_at(0, 0) == red);
    EXPECT(parted->color_at(2, 2).green == 255);

    // blit stays inside everything.
    auto target = white_bitmap(4, 4);
    Painter blitter(*target);
    blitter.blit(*source, { 2, 2 }, source->rect());
    blitter.blit(*source, { -3, -3 }, source->rect());
    blitter.blit(*source, { 100, 100 }, source->rect());
    EXPECT(target->color_at(3, 3) == red);
    EXPECT(target->color_at(0, 0) == red);
}

TEST_CASE(masks)
{
    auto mask = MUST(AlphaMask::create({ 3, 1 }));
    mask->scanline(0)[0] = 0;
    mask->scanline(0)[1] = 128;
    mask->scanline(0)[2] = 255;
    auto bitmap = white_bitmap(8, 2);
    Painter painter(*bitmap);
    painter.draw_mask(*mask, { 1, 1 }, black);
    EXPECT(bitmap->color_at(1, 1).red == 255);
    EXPECT(near(bitmap->color_at(2, 1).red, 127));
    EXPECT(bitmap->color_at(3, 1).red == 0);
    EXPECT(bitmap->color_at(4, 1).red == 255);
    // Outside, or cut by the bitmap's edge: fine.
    painter.draw_mask(*mask, { -2, 0 }, black);
    EXPECT(bitmap->color_at(0, 0).red == 0);
    painter.draw_mask(*mask, { 7, 0 }, black);
    painter.draw_mask(*mask, { 50, 50 }, black);
}
