/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Filter.h>
#include <LibTest/TestCase.h>

using namespace Gfx;
namespace Math = Base::Math;

TEST_CASE(blur_spreads_and_keeps_the_total)
{
    auto mask = MUST(AlphaMask::create({ 101, 101 }));
    for (int y = 40; y <= 60; ++y) {
        for (int x = 40; x <= 60; ++x)
            mask->scanline(y)[x] = 255;
    }
    u64 before = 0;
    for (int y = 0; y < 101; ++y) {
        for (int x = 0; x < 101; ++x)
            before += mask->scanline(y)[x];
    }
    MUST(gaussian_blur(*mask, 5));
    u64 after = 0;
    for (int y = 0; y < 101; ++y) {
        for (int x = 0; x < 101; ++x)
            after += mask->scanline(y)[x];
    }
    // Rounding aside, nothing is gained or lost.
    EXPECT(Math::absolute(static_cast<double>(after) - static_cast<double>(before)) < static_cast<double>(before) * 0.02);
    // The edge of the square: about half; the middle a little down; well
    // outside, nothing.
    EXPECT(mask->scanline(50)[50] > 200);
    EXPECT(mask->scanline(50)[40] > 90 && mask->scanline(50)[40] < 165);
    EXPECT_EQ(mask->scanline(50)[5], 0);
    // Symmetric across both axes and the diagonal.
    for (int i = 0; i < 101; ++i) {
        EXPECT(Math::absolute(mask->scanline(50)[i] - mask->scanline(50)[100 - i]) <= 2);
        EXPECT(Math::absolute(mask->scanline(i)[50] - mask->scanline(50)[i]) <= 2);
    }
}

TEST_CASE(blur_approximates_a_gaussian)
{
    // A single impulse spreads to about the bell curve's shape.
    constexpr int size = 121;
    auto mask = MUST(AlphaMask::create({ size, 1 }));
    auto bitmap = MUST(Bitmap::create({ size, 1 }));
    (void)mask;
    for (int x = 0; x < size; ++x)
        bitmap->set_pixel_at(x, 0, x < size / 2 ? 0 : 0xffffffffu);
    MUST(gaussian_blur(*bitmap, bitmap->rect(), 6));
    // One sigma past a step, a Gaussian has reached 84%: to within a few percent.
    EXPECT(pixel_alpha(bitmap->pixel_at(size / 2 + 6, 0)) > 200);
    EXPECT(pixel_alpha(bitmap->pixel_at(size / 2 + 6, 0)) < 228);
    EXPECT(pixel_alpha(bitmap->pixel_at(size / 2 - 6, 0)) > 27);
    EXPECT(pixel_alpha(bitmap->pixel_at(size / 2 - 6, 0)) < 55);
    // Rising all the way.
    for (int x = 1; x < size; ++x)
        EXPECT(pixel_alpha(bitmap->pixel_at(x, 0)) >= pixel_alpha(bitmap->pixel_at(x - 1, 0)));
}

TEST_CASE(blur_of_flat_color_is_the_same)
{
    auto bitmap = MUST(Bitmap::create({ 32, 32 }));
    auto color = Color::from_rgb(0x4080c0);
    bitmap->clear(color);
    MUST(gaussian_blur(*bitmap, bitmap->rect(), 8));
    for (int y = 0; y < 32; y += 7) {
        for (int x = 0; x < 32; x += 7) {
            auto got = bitmap->color_at(x, y);
            EXPECT(Math::absolute(got.red - color.red) <= 1 && Math::absolute(got.green - color.green) <= 1 && Math::absolute(got.blue - color.blue) <= 1);
        }
    }
}

TEST_CASE(blur_stays_in_its_region)
{
    auto bitmap = MUST(Bitmap::create({ 32, 32 }));
    bitmap->clear(Color::from_rgb(0xffffff));
    for (int x = 0; x < 32; ++x)
        bitmap->set_pixel_at(x, 16, make_pixel(0, 0, 0, 255));
    IntRect region { 8, 8, 16, 16 };
    MUST(gaussian_blur(*bitmap, region, 3));
    EXPECT_EQ(bitmap->pixel_at(4, 16), make_pixel(0, 0, 0, 255));
    EXPECT_EQ(bitmap->pixel_at(4, 15), make_pixel(255, 255, 255, 255));
    EXPECT(pixel_red(bitmap->pixel_at(16, 16)) > 100);
    // A negligible sigma, an empty or outside region: no change, no failure.
    auto before = bitmap->pixel_at(16, 16);
    MUST(gaussian_blur(*bitmap, region, 0.2f));
    MUST(gaussian_blur(*bitmap, IntRect { 100, 100, 5, 5 }, 5));
    EXPECT_EQ(bitmap->pixel_at(16, 16), before);
}

TEST_CASE(saturation)
{
    auto bitmap = MUST(Bitmap::create({ 1, 1 }));
    auto reset = [&] { bitmap->clear(Color::from_rgb(0xc04020)); };

    reset();
    adjust_saturation(*bitmap, bitmap->rect(), 1);
    auto same = bitmap->color_at(0, 0);
    EXPECT(Math::absolute(same.red - 0xc0) <= 1 && Math::absolute(same.green - 0x40) <= 1 && Math::absolute(same.blue - 0x20) <= 1);

    reset();
    adjust_saturation(*bitmap, bitmap->rect(), 0);
    auto grey = bitmap->color_at(0, 0);
    EXPECT(Math::absolute(grey.red - grey.green) <= 1 && Math::absolute(grey.green - grey.blue) <= 1);
    EXPECT(grey.red > 0x50 && grey.red < 0x70);

    // More vivid: the bright channel up, the dull ones down.
    reset();
    adjust_saturation(*bitmap, bitmap->rect(), 1.8f);
    auto vivid = bitmap->color_at(0, 0);
    EXPECT(vivid.red > 0xc0);
    EXPECT(vivid.green < 0x40);
    EXPECT(vivid.blue < 0x20);

    // Premultiplied values stay valid.
    bitmap->clear(Color::from_rgb(0xff0000, 100));
    adjust_saturation(*bitmap, bitmap->rect(), 3);
    auto pixel = bitmap->pixel_at(0, 0);
    EXPECT(pixel_red(pixel) <= pixel_alpha(pixel));
    EXPECT_EQ(pixel_alpha(pixel), 100);
}
