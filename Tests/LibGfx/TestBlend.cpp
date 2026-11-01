/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/FastRandom.h>
#include <LibBase/Vector.h>
#include <LibGfx/Blend.h>
#include <LibTest/TestCase.h>

using namespace Gfx;

namespace {

// Random premultiplied pixels (no channel above alpha).
Pixel random_pixel(u64& state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    u8 alpha = static_cast<u8>(state >> 56);
    auto below = [&](int shift) { return static_cast<u8>(((state >> shift) & 0xff) * (static_cast<u32>(alpha) + 1) >> 8); };
    return make_pixel(below(8), below(16), below(24), alpha);
}

}

TEST_CASE(division_by_255_is_exact)
{
    for (u32 x = 0; x <= 255 * 255; ++x) {
        u32 expected = (x * 2 + 255) / 510;
        EXPECT_EQ(divide_by_255(x), expected);
    }
}

TEST_CASE(one_pixel_cases)
{
    Pixel red = make_pixel(255, 0, 0, 255);
    Pixel blue = make_pixel(0, 0, 255, 255);
    EXPECT_EQ(blend_over(red, blue), red);
    EXPECT_EQ(blend_over(0, blue), blue);
    EXPECT_EQ(blend_over(make_pixel(128, 0, 0, 128), blue), make_pixel(128, 0, 127, 255));
    EXPECT_EQ(scale_pixel(red, 255), red);
    EXPECT_EQ(scale_pixel(red, 0), 0u);
}

TEST_CASE(colors_and_pixels)
{
    auto color = Color::from_rgb(0x336699);
    EXPECT_EQ(color.red, 0x33);
    EXPECT_EQ(color.blue, 0x99);
    EXPECT_EQ(color.to_pixel(), make_pixel(0x33, 0x66, 0x99, 255));
    auto half = color.with_alpha(128);
    EXPECT_EQ(half.to_pixel(), make_pixel(26, 51, 77, 128));
    auto back = Color::from_pixel(half.to_pixel());
    EXPECT_EQ(back.alpha, 128);
    EXPECT(back.red >= 0x32 && back.red <= 0x34);
    EXPECT(Color::from_pixel(0) == (Color { 0, 0, 0, 0 }));
}

// Every row function gives exactly what the one-pixel formula does, whatever
// the count (the vector loops have a scalar tail).
TEST_CASE(rows_match_the_formula)
{
    u64 state = 42;
    for (int count = 0; count < 33; ++count) {
        Vector<Pixel> source, destination, expected;
        Vector<u8> coverage;
        for (int i = 0; i < count; ++i) {
            source.append(random_pixel(state));
            destination.append(random_pixel(state));
            state = state * 6364136223846793005ull + 1;
            coverage.append(static_cast<u8>((i % 5) == 0 ? 0 : state >> 56));
        }
        auto reset = [&] {
            expected = destination;
        };

        reset();
        blend_row(expected.data(), source.data(), count);
        for (int i = 0; i < count; ++i)
            EXPECT_EQ(expected[static_cast<size_t>(i)], blend_over(source[static_cast<size_t>(i)], destination[static_cast<size_t>(i)]));

        reset();
        blend_row_opacity(expected.data(), source.data(), 100, count);
        for (int i = 0; i < count; ++i)
            EXPECT_EQ(expected[static_cast<size_t>(i)], blend_over(scale_pixel(source[static_cast<size_t>(i)], 100), destination[static_cast<size_t>(i)]));

        reset();
        blend_row_coverage(expected.data(), source.data(), coverage.data(), count);
        for (int i = 0; i < count; ++i)
            EXPECT_EQ(expected[static_cast<size_t>(i)], blend_over(scale_pixel(source[static_cast<size_t>(i)], coverage[static_cast<size_t>(i)]), destination[static_cast<size_t>(i)]));

        Pixel solid = random_pixel(state);
        reset();
        blend_row_solid(expected.data(), solid, count);
        for (int i = 0; i < count; ++i)
            EXPECT_EQ(expected[static_cast<size_t>(i)], blend_over(solid, destination[static_cast<size_t>(i)]));

        reset();
        blend_row_solid_coverage(expected.data(), solid, coverage.data(), count);
        for (int i = 0; i < count; ++i) {
            auto cover = coverage[static_cast<size_t>(i)];
            Pixel wanted = cover ? blend_over(scale_pixel(solid, cover), destination[static_cast<size_t>(i)]) : destination[static_cast<size_t>(i)];
            EXPECT_EQ(expected[static_cast<size_t>(i)], wanted);
        }
    }
}
