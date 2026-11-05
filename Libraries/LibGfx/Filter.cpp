/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/New.h>
#include <LibBase/Vector.h>
#include <LibGfx/Filter.h>

namespace Gfx {

namespace {

// Three boxes approximating a Gaussian of this sigma: their reach to each
// side (before and after the pixel).
struct Box {
    int before;
    int after;
};

int box_width_for(float sigma)
{
    constexpr double factor = 1.87997120597325; // 3 sqrt(2 pi) / 4
    return static_cast<int>(static_cast<double>(sigma) * factor + 0.5);
}

void boxes_for(float sigma, Box boxes[3])
{
    int width = box_width_for(sigma);
    if (width % 2 == 1) {
        int reach = width / 2;
        boxes[0] = boxes[1] = boxes[2] = { reach, reach };
    } else {
        // Two even boxes shifted opposite ways, and an odd one a pixel wider.
        int reach = width / 2;
        boxes[0] = { reach, reach - 1 };
        boxes[1] = { reach - 1, reach };
        boxes[2] = { reach, reach };
    }
}

// Sums to values: (sum * reciprocal + half) >> 16, for a window this wide.
constexpr u32 reciprocal_for(int width)
{
    return static_cast<u32>(65536 / width);
}

using u32x4 = u32 __attribute__((vector_size(16)));
using u8x4 = u8 __attribute__((vector_size(4)));

[[gnu::always_inline]] inline u32x4 widen(Pixel pixel)
{
    u8x4 narrow;
    __builtin_memcpy(&narrow, &pixel, 4);
    return __builtin_convertvector(narrow, u32x4);
}

[[gnu::always_inline]] inline Pixel narrow_divided(u32x4 sum, u32 reciprocal)
{
    u32x4 divided = (sum * reciprocal + 32768u) >> 16;
    u8x4 narrow = __builtin_convertvector(divided, u8x4);
    Pixel pixel;
    __builtin_memcpy(&pixel, &narrow, 4);
    return pixel;
}

// One box along a row or column: `count` elements `stride` apart, source to
// destination.
void box_pixels(Pixel const* source, Pixel* destination, int count, ptrdiff_t source_stride, ptrdiff_t destination_stride, Box box)
{
    int width = box.before + box.after + 1;
    u32 reciprocal = reciprocal_for(width);
    auto at = [&](int index) { return source[clamp(index, 0, count - 1) * source_stride]; };
    u32x4 sum = widen(source[0]) * static_cast<u32>(box.before + 1);
    for (int i = 1; i <= box.after; ++i)
        sum += widen(at(i));
    for (int i = 0; i < count; ++i) {
        destination[i * destination_stride] = narrow_divided(sum, reciprocal);
        sum += widen(at(i + box.after + 1));
        sum -= widen(at(i - box.before));
    }
}

void box_values(u8 const* source, u8* destination, int count, ptrdiff_t source_stride, ptrdiff_t destination_stride, Box box)
{
    int width = box.before + box.after + 1;
    u32 reciprocal = reciprocal_for(width);
    auto at = [&](int index) { return static_cast<u32>(source[clamp(index, 0, count - 1) * source_stride]); };
    u32 sum = at(0) * static_cast<u32>(box.before + 1);
    for (int i = 1; i <= box.after; ++i)
        sum += at(i);
    for (int i = 0; i < count; ++i) {
        destination[i * destination_stride] = static_cast<u8>((sum * reciprocal + 32768u) >> 16);
        sum += at(i + box.after + 1);
        sum -= at(i - box.before);
    }
}

}

ErrorOr<void> gaussian_blur(Bitmap& bitmap, IntRect region, float sigma, Bitmap* spare_bitmap)
{
    region = region.intersected(bitmap.rect());
    if (region.is_empty() || box_width_for(sigma) < 2)
        return {};
    Box boxes[3];
    boxes_for(sigma, boxes);
    RefPtr<Bitmap> scratch = spare_bitmap;
    if (!scratch || scratch->width() < region.width || scratch->height() < region.height)
        scratch = TRY(Bitmap::create(region.size()));
    ptrdiff_t pitch = bitmap.pitch();
    // Across, into the scratch space and back, three times; down likewise.
    auto* area = bitmap.scanline(region.y) + region.x;
    auto* spare = scratch->pixels();
    ptrdiff_t spare_pitch = scratch->pitch();
    for (auto const& box : boxes) {
        for (int y = 0; y < region.height; ++y)
            box_pixels(area + y * pitch, spare + y * spare_pitch, region.width, 1, 1, box);
        for (int x = 0; x < region.width; ++x)
            box_pixels(spare + x, area + x, region.height, spare_pitch, pitch, box);
    }
    return {};
}

ErrorOr<void> gaussian_blur(AlphaMask& mask, float sigma)
{
    if (mask.size().is_empty() || box_width_for(sigma) < 2)
        return {};
    Box boxes[3];
    boxes_for(sigma, boxes);
    auto scratch = TRY(AlphaMask::create(mask.size()));
    ptrdiff_t pitch = mask.width();
    auto* area = mask.scanline(0);
    auto* spare = scratch->scanline(0);
    for (auto const& box : boxes) {
        for (int y = 0; y < mask.height(); ++y)
            box_values(area + y * pitch, spare + y * pitch, mask.width(), 1, 1, box);
        for (int x = 0; x < mask.width(); ++x)
            box_values(spare + x, area + x, mask.height(), pitch, pitch, box);
    }
    return {};
}

void adjust_saturation(Bitmap& bitmap, IntRect region, float saturation)
{
    region = region.intersected(bitmap.rect());
    // CSS's matrix, in 8.8 fixed point. Linear, so premultiplied values do.
    float s = saturation;
    auto fixed = [](float value) { return static_cast<i32>(value * 256.0f + (value < 0 ? -0.5f : 0.5f)); };
    i32 rr = fixed(0.213f + 0.787f * s), rg = fixed(0.715f - 0.715f * s), rb = fixed(0.072f - 0.072f * s);
    i32 gr = fixed(0.213f - 0.213f * s), gg = fixed(0.715f + 0.285f * s), gb = fixed(0.072f - 0.072f * s);
    i32 br = fixed(0.213f - 0.213f * s), bg = fixed(0.715f - 0.715f * s), bb = fixed(0.072f + 0.928f * s);
    for (int y = region.y; y < region.bottom(); ++y) {
        auto* row = bitmap.scanline(y);
        for (int x = region.x; x < region.right(); ++x) {
            Pixel pixel = row[x];
            i32 r = pixel_red(pixel), g = pixel_green(pixel), b = pixel_blue(pixel);
            i32 a = pixel_alpha(pixel);
            auto limit = [a](i32 value) { return static_cast<u8>(clamp((value + 128) >> 8, 0, a)); };
            row[x] = make_pixel(limit(rr * r + rg * g + rb * b), limit(gr * r + gg * g + gb * b), limit(br * r + bg * g + bb * b), static_cast<u8>(a));
        }
    }
}

}
