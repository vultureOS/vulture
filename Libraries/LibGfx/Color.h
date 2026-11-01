/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Gfx {

// A pixel in a Bitmap: blue, green, red, alpha in memory, its color channels
// already multiplied by alpha.
using Pixel = u32;

[[nodiscard]] constexpr Pixel make_pixel(u8 red, u8 green, u8 blue, u8 alpha)
{
    return static_cast<u32>(blue) | (static_cast<u32>(green) << 8) | (static_cast<u32>(red) << 16) | (static_cast<u32>(alpha) << 24);
}

[[nodiscard]] constexpr u8 pixel_blue(Pixel pixel) { return static_cast<u8>(pixel); }
[[nodiscard]] constexpr u8 pixel_green(Pixel pixel) { return static_cast<u8>(pixel >> 8); }
[[nodiscard]] constexpr u8 pixel_red(Pixel pixel) { return static_cast<u8>(pixel >> 16); }
[[nodiscard]] constexpr u8 pixel_alpha(Pixel pixel) { return static_cast<u8>(pixel >> 24); }

// x / 255, rounded to nearest, for x up to 255 * 255.
[[nodiscard]] constexpr u32 divide_by_255(u32 x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

// A color as people state them: not premultiplied.
struct Color {
    u8 red { 0 };
    u8 green { 0 };
    u8 blue { 0 };
    u8 alpha { 255 };

    // 0xRRGGBB.
    [[nodiscard]] static constexpr Color from_rgb(u32 rgb, u8 alpha = 255)
    {
        return { static_cast<u8>(rgb >> 16), static_cast<u8>(rgb >> 8), static_cast<u8>(rgb), alpha };
    }

    [[nodiscard]] static constexpr Color from_pixel(Pixel pixel)
    {
        u32 a = pixel_alpha(pixel);
        if (a == 0)
            return { 0, 0, 0, 0 };
        auto unmultiply = [a](u32 channel) { return static_cast<u8>((channel * 255 + a / 2) / a); };
        return { unmultiply(pixel_red(pixel)), unmultiply(pixel_green(pixel)), unmultiply(pixel_blue(pixel)), static_cast<u8>(a) };
    }

    [[nodiscard]] constexpr Color with_alpha(u8 new_alpha) const { return { red, green, blue, new_alpha }; }

    // Its alpha scaled by `factor` (0 to 1).
    [[nodiscard]] constexpr Color with_opacity(float factor) const
    {
        return { red, green, blue, static_cast<u8>(static_cast<float>(alpha) * factor + 0.5f) };
    }

    [[nodiscard]] constexpr Pixel to_pixel() const
    {
        return make_pixel(static_cast<u8>(divide_by_255(red * static_cast<u32>(alpha))), static_cast<u8>(divide_by_255(green * static_cast<u32>(alpha))),
            static_cast<u8>(divide_by_255(blue * static_cast<u32>(alpha))), alpha);
    }

    constexpr bool operator==(Color const&) const = default;
};

}
