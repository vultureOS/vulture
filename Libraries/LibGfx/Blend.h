/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibGfx/Color.h>

// Rows of pixels, composited. These are the inner loops of everything that
// draws. They use the compiler's vector types (NEON here, SSE on an x86 host)
// and give exactly the result of the one-pixel formula.

namespace Gfx {

// Source over destination, one pixel.
[[nodiscard]] constexpr Pixel blend_over(Pixel source, Pixel destination)
{
    u32 inverse = 255 - pixel_alpha(source);
    return make_pixel(
        static_cast<u8>(pixel_red(source) + divide_by_255(pixel_red(destination) * inverse)),
        static_cast<u8>(pixel_green(source) + divide_by_255(pixel_green(destination) * inverse)),
        static_cast<u8>(pixel_blue(source) + divide_by_255(pixel_blue(destination) * inverse)),
        static_cast<u8>(pixel_alpha(source) + divide_by_255(pixel_alpha(destination) * inverse)));
}

// All four channels times a factor (0 to 255).
[[nodiscard]] constexpr Pixel scale_pixel(Pixel pixel, u32 factor)
{
    return make_pixel(static_cast<u8>(divide_by_255(pixel_red(pixel) * factor)), static_cast<u8>(divide_by_255(pixel_green(pixel) * factor)),
        static_cast<u8>(divide_by_255(pixel_blue(pixel) * factor)), static_cast<u8>(divide_by_255(pixel_alpha(pixel) * factor)));
}

void fill_row(Pixel* destination, Pixel value, int count);
// `value` over each destination pixel.
void blend_row_solid(Pixel* destination, Pixel value, int count);
// `value`, scaled by each coverage value (0 to 255), over each pixel.
void blend_row_solid_coverage(Pixel* destination, Pixel value, u8 const* coverage, int count);
// Each source pixel over each destination pixel.
void blend_row(Pixel* destination, Pixel const* source, int count);
// The same, the source scaled by `opacity` (0 to 255).
void blend_row_opacity(Pixel* destination, Pixel const* source, u32 opacity, int count);
// The same, the source scaled by each coverage value.
void blend_row_coverage(Pixel* destination, Pixel const* source, u8 const* coverage, int count);

}
