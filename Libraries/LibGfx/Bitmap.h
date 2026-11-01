/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/RefPtr.h>
#include <LibGfx/Color.h>
#include <LibGfx/Geometry.h>

namespace Gfx {

// Pixels: its own memory, or someone else's (a window's shared surface, the
// screen). `pitch` is in pixels. Scanlines run top to bottom.
class Bitmap final : public RefCounted<Bitmap> {
public:
    // Fully transparent. ENOMEM if it does not fit.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> create(IntSize);
    // Pixels that outlive this object and belong to the caller.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Bitmap>> wrap(Pixel* pixels, IntSize, int pitch);
    ~Bitmap();

    [[nodiscard]] IntSize size() const { return m_size; }
    [[nodiscard]] int width() const { return m_size.width; }
    [[nodiscard]] int height() const { return m_size.height; }
    [[nodiscard]] int pitch() const { return m_pitch; }
    [[nodiscard]] IntRect rect() const { return { 0, 0, m_size.width, m_size.height }; }
    [[nodiscard]] Pixel* pixels() { return m_pixels; }
    [[nodiscard]] Pixel const* pixels() const { return m_pixels; }
    [[nodiscard]] Pixel* scanline(int y) { return m_pixels + static_cast<ptrdiff_t>(y) * m_pitch; }
    [[nodiscard]] Pixel const* scanline(int y) const { return m_pixels + static_cast<ptrdiff_t>(y) * m_pitch; }

    [[nodiscard]] Pixel pixel_at(int x, int y) const { return scanline(y)[x]; }
    void set_pixel_at(int x, int y, Pixel pixel) { scanline(y)[x] = pixel; }
    [[nodiscard]] Color color_at(int x, int y) const { return Color::from_pixel(pixel_at(x, y)); }

    // Every pixel set to this color (not blended).
    void clear(Color = { 0, 0, 0, 0 });

    // A copy of a part of it.
    [[nodiscard]] ErrorOr<NonnullRefPtr<Bitmap>> cropped(IntRect) const;
    // Half the size in each direction, each new pixel the average of four.
    [[nodiscard]] ErrorOr<NonnullRefPtr<Bitmap>> halved() const;

private:
    Bitmap(Pixel*, IntSize, int pitch, bool owns);

    Pixel* m_pixels { nullptr };
    IntSize m_size;
    int m_pitch { 0 };
    bool m_owns_pixels { false };
};

// One byte per pixel: coverage, a glyph, a shadow.
class AlphaMask final : public RefCounted<AlphaMask> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<AlphaMask>> create(IntSize);
    ~AlphaMask();

    [[nodiscard]] IntSize size() const { return m_size; }
    [[nodiscard]] int width() const { return m_size.width; }
    [[nodiscard]] int height() const { return m_size.height; }
    [[nodiscard]] u8* scanline(int y) { return m_values + static_cast<ptrdiff_t>(y) * m_size.width; }
    [[nodiscard]] u8 const* scanline(int y) const { return m_values + static_cast<ptrdiff_t>(y) * m_size.width; }

private:
    AlphaMask(u8*, IntSize);

    u8* m_values { nullptr };
    IntSize m_size;
};

}
