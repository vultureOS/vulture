/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>

namespace WindowServer {

class WorkerPool;

// One display, as far as anything else is concerned. The main one is at 0,0, as on a Mac, and the others around it, so one to its
// left or above it has negative coordinates.
struct Display {
    int output { 0 };
    Gfx::IntRect points;
    // What it really has, which isn't the points times the scale when it "looks like" something else.
    Gfx::IntSize panel;
};

// The displays, and the picture of the desktop that the compositor draws in. With one display showing just what's drawn, that picture
// is the display's own framebuffer, which the device only looks at when told. With more, or scaled, or tinted, it's memory of its own,
// and each display gets its part of it when it's flushed.
class Screen {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Screen>> open();
    ~Screen();

    [[nodiscard]] Gfx::Bitmap& framebuffer() { return *m_framebuffer; }
    // Of all the desktop, in the picture.
    [[nodiscard]] Gfx::IntSize pixels() const { return m_framebuffer->size(); }
    [[nodiscard]] int scale() const { return m_scale; }
    // How far along the picture the main display is, in points: where 0,0 is. Nothing's at negative coordinates in the picture.
    [[nodiscard]] Gfx::IntPoint origin() const { return m_origin; }

    [[nodiscard]] Vector<Display> const& displays() const { return m_displays; }
    [[nodiscard]] Display const& main_display() const { return m_displays[0]; }
    // The one it's on, or else the nearest.
    [[nodiscard]] Display const& display_at(Gfx::IntPoint) const;
    // The one a pointer device is bound to.
    [[nodiscard]] Display const& display_of_head(int head) const;
    [[nodiscard]] Gfx::IntPoint nearest_point_on_a_display(Gfx::IntPoint) const;

    // Where each output's to be, and how big it's to look. Taking up its new place needs everything drawn again.
    struct Placement {
        int output { 0 };
        Gfx::IntPoint origin;
        Gfx::IntSize points;
    };
    [[nodiscard]] ErrorOr<void> arrange(Span<Placement const>, bool mirrored);
    [[nodiscard]] bool is_mirrored() const { return m_mirrored; }
    [[nodiscard]] size_t output_count() const { return m_outputs.size(); }
    // Warmer: 0 is not at all, 1 is as far as it goes.
    [[nodiscard]] ErrorOr<void> set_warmth(float);
    [[nodiscard]] float warmth() const { return m_warmth; }

    void flush(Gfx::IntRect, WorkerPool&);
    void set_cursor(Gfx::Bitmap const&, Gfx::IntPoint hot_spot);
    // In the picture's pixels.
    void move_cursor(Gfx::IntPoint pixel);

private:
    struct Output {
        int fd { -1 };
        void* mapping { nullptr };
        size_t mapping_size { 0 };
        RefPtr<Gfx::Bitmap> panel;
        Gfx::IntPoint origin;
        Gfx::IntSize points;
        // Its part of the picture.
        Gfx::IntRect area;
    };

    Screen() = default;
    [[nodiscard]] ErrorOr<void> rebuild();
    void copy_rows(Output const&, int first_row, int end_row, Gfx::IntRect damage) const;
    void flush_output(Output const&, Gfx::IntRect damage, WorkerPool&);
    [[nodiscard]] Output const* output_at(Gfx::IntPoint pixel) const;

    Vector<Output> m_outputs;
    RefPtr<Gfx::Bitmap> m_framebuffer;
    Vector<Display> m_displays;
    int m_scale { 1 };
    Gfx::IntPoint m_origin;
    bool m_mirrored { false };
    // The picture is the first display's framebuffer.
    bool m_direct { true };
    float m_warmth { 0 };
    u8 m_warm_red[256] {};
    u8 m_warm_green[256] {};
    u8 m_warm_blue[256] {};

    int m_cursor_output { 0 };
    Vector<Gfx::Pixel> m_cursor_pixels;
    Gfx::IntPoint m_cursor_hot_spot;
};

}
