/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/New.h>
#include <LibGfx/Bitmap.h>

namespace Gfx {

namespace {

// The largest side anything here expects (and what keeps the sizes below
// from overflowing).
constexpr int max_dimension = 32768;

bool is_valid_size(IntSize size)
{
    return size.width >= 0 && size.height >= 0 && size.width <= max_dimension && size.height <= max_dimension;
}

}

Bitmap::Bitmap(Pixel* pixels, IntSize size, int pitch, bool owns)
    : m_pixels(pixels)
    , m_size(size)
    , m_pitch(pitch)
    , m_owns_pixels(owns)
{
}

Bitmap::~Bitmap()
{
    if (m_owns_pixels)
        delete[] m_pixels;
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::create(IntSize size)
{
    if (!is_valid_size(size))
        return Error::from_errno(EINVAL);
    size_t count = static_cast<size_t>(size.width) * static_cast<size_t>(size.height);
    auto* pixels = new (Base::nothrow) Pixel[count];
    if (!pixels)
        return Error::from_errno(ENOMEM);
    __builtin_memset(pixels, 0, count * sizeof(Pixel));
    auto* bitmap = new (Base::nothrow) Bitmap(pixels, size, size.width, true);
    if (!bitmap) {
        delete[] pixels;
        return Error::from_errno(ENOMEM);
    }
    return adopt_ref(*bitmap);
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::wrap(Pixel* pixels, IntSize size, int pitch)
{
    if (!is_valid_size(size) || pitch < size.width)
        return Error::from_errno(EINVAL);
    auto* bitmap = new (Base::nothrow) Bitmap(pixels, size, pitch, false);
    if (!bitmap)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*bitmap);
}

void Bitmap::clear(Color color)
{
    Pixel value = color.to_pixel();
    for (int y = 0; y < height(); ++y) {
        auto* row = scanline(y);
        for (int x = 0; x < width(); ++x)
            row[x] = value;
    }
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::cropped(IntRect area) const
{
    area = area.intersected(rect());
    auto copy = TRY(create(area.size()));
    for (int y = 0; y < area.height; ++y)
        __builtin_memcpy(copy->scanline(y), scanline(area.y + y) + area.x, static_cast<size_t>(area.width) * sizeof(Pixel));
    return copy;
}

ErrorOr<NonnullRefPtr<Bitmap>> Bitmap::halved() const
{
    IntSize new_size { max(width() / 2, 1), max(height() / 2, 1) };
    auto result = TRY(create(new_size));
    for (int y = 0; y < new_size.height; ++y) {
        auto const* upper = scanline(min(y * 2, height() - 1));
        auto const* lower = scanline(min(y * 2 + 1, height() - 1));
        auto* row = result->scanline(y);
        for (int x = 0; x < new_size.width; ++x) {
            int left = min(x * 2, width() - 1);
            int right = min(x * 2 + 1, width() - 1);
            auto average = [&](auto channel) {
                return static_cast<u8>((channel(upper[left]) + channel(upper[right]) + channel(lower[left]) + channel(lower[right]) + 2u) / 4u);
            };
            row[x] = make_pixel(average([](Pixel p) { return static_cast<u32>(pixel_red(p)); }), average([](Pixel p) { return static_cast<u32>(pixel_green(p)); }),
                average([](Pixel p) { return static_cast<u32>(pixel_blue(p)); }), average([](Pixel p) { return static_cast<u32>(pixel_alpha(p)); }));
        }
    }
    return result;
}

AlphaMask::AlphaMask(u8* values, IntSize size)
    : m_values(values)
    , m_size(size)
{
}

AlphaMask::~AlphaMask()
{
    delete[] m_values;
}

ErrorOr<NonnullRefPtr<AlphaMask>> AlphaMask::create(IntSize size)
{
    if (!is_valid_size(size))
        return Error::from_errno(EINVAL);
    size_t count = static_cast<size_t>(size.width) * static_cast<size_t>(size.height);
    auto* values = new (Base::nothrow) u8[count];
    if (!values)
        return Error::from_errno(ENOMEM);
    __builtin_memset(values, 0, count);
    auto* mask = new (Base::nothrow) AlphaMask(values, size);
    if (!mask) {
        delete[] values;
        return Error::from_errno(ENOMEM);
    }
    return adopt_ref(*mask);
}

}
