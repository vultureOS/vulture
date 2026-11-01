/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibGfx/Blend.h>

namespace Gfx {

namespace {

// Two pixels at a time: eight channels, widened to sixteen bits.
using u8x8 = u8 __attribute__((vector_size(8)));
using u16x8 = u16 __attribute__((vector_size(16)));

[[gnu::always_inline]] inline u16x8 load_widened(Pixel const* pixels)
{
    u8x8 narrow;
    __builtin_memcpy(&narrow, pixels, sizeof(narrow));
    return __builtin_convertvector(narrow, u16x8);
}

[[gnu::always_inline]] inline void store_narrowed(Pixel* pixels, u16x8 wide)
{
    u8x8 narrow = __builtin_convertvector(wide, u8x8);
    __builtin_memcpy(pixels, &narrow, sizeof(narrow));
}

// divide_by_255 in each channel.
[[gnu::always_inline]] inline u16x8 divide_by_255_each(u16x8 x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

// Each pixel's alpha in all of its channels.
[[gnu::always_inline]] inline u16x8 alphas_of(u16x8 wide)
{
    return __builtin_shufflevector(wide, wide, 3, 3, 3, 3, 7, 7, 7, 7);
}

[[gnu::always_inline]] inline u16x8 over(u16x8 source, u16x8 destination)
{
    u16x8 inverse = 255 - alphas_of(source);
    return source + divide_by_255_each(destination * inverse);
}

[[gnu::always_inline]] inline u16x8 repeated(Pixel value)
{
    Pixel both[2] = { value, value };
    return load_widened(both);
}

// Two coverage values, each in the four channels of its pixel.
[[gnu::always_inline]] inline u16x8 coverage_pair(u8 const* coverage)
{
    u16 first = coverage[0];
    u16 second = coverage[1];
    return u16x8 { first, first, first, first, second, second, second, second };
}

}

void fill_row(Pixel* destination, Pixel value, int count)
{
    for (int i = 0; i < count; ++i)
        destination[i] = value;
}

void blend_row_solid(Pixel* destination, Pixel value, int count)
{
    if (pixel_alpha(value) == 255) {
        fill_row(destination, value, count);
        return;
    }
    if (pixel_alpha(value) == 0 && value == 0)
        return;
    u16x8 source = repeated(value);
    int i = 0;
    for (; i + 2 <= count; i += 2)
        store_narrowed(destination + i, over(source, load_widened(destination + i)));
    if (i < count)
        destination[i] = blend_over(value, destination[i]);
}

void blend_row_solid_coverage(Pixel* destination, Pixel value, u8 const* coverage, int count)
{
    u16x8 source = repeated(value);
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        u16 pair;
        __builtin_memcpy(&pair, coverage + i, 2);
        if (pair == 0)
            continue;
        u16x8 scaled = divide_by_255_each(source * coverage_pair(coverage + i));
        store_narrowed(destination + i, over(scaled, load_widened(destination + i)));
    }
    if (i < count && coverage[i])
        destination[i] = blend_over(scale_pixel(value, coverage[i]), destination[i]);
}

void blend_row(Pixel* destination, Pixel const* source, int count)
{
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        u64 pair;
        __builtin_memcpy(&pair, source + i, 8);
        // Wholly opaque or wholly clear: most pixels, and no arithmetic.
        constexpr u64 alphas = 0xff000000ff000000ull;
        if ((pair & alphas) == alphas) {
            __builtin_memcpy(destination + i, &pair, 8);
        } else if (pair != 0) {
            store_narrowed(destination + i, over(load_widened(source + i), load_widened(destination + i)));
        }
    }
    if (i < count)
        destination[i] = blend_over(source[i], destination[i]);
}

void blend_row_opacity(Pixel* destination, Pixel const* source, u32 opacity, int count)
{
    if (opacity >= 255) {
        blend_row(destination, source, count);
        return;
    }
    u16 factor = static_cast<u16>(opacity);
    u16x8 factors { factor, factor, factor, factor, factor, factor, factor, factor };
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        u16x8 scaled = divide_by_255_each(load_widened(source + i) * factors);
        store_narrowed(destination + i, over(scaled, load_widened(destination + i)));
    }
    if (i < count)
        destination[i] = blend_over(scale_pixel(source[i], opacity), destination[i]);
}

void blend_row_coverage(Pixel* destination, Pixel const* source, u8 const* coverage, int count)
{
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        u16x8 scaled = divide_by_255_each(load_widened(source + i) * coverage_pair(coverage + i));
        store_narrowed(destination + i, over(scaled, load_widened(destination + i)));
    }
    if (i < count)
        destination[i] = blend_over(scale_pixel(source[i], coverage[i]), destination[i]);
}

}
