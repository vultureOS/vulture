/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Vector.h>
#include <LibGfx/Geometry.h>
#include <LibGfx/Path.h>

namespace Gfx {

enum class FillRule : u8 {
    NonZero,
    EvenOdd,
};

// Turns edges into coverage: for each pixel, the exact area of it inside the
// shape (no sampling), one scanline at a time. Each edge adds the signed area
// it sweeps to the cells it crosses; a running sum along the row then gives
// the winding at each pixel.
class Rasterizer {
public:
    explicit Rasterizer(IntRect clip);

    // In the same coordinates as the clip. Subpaths close themselves.
    // Curves stay within `tolerance` of a pixel: a fortieth, so an edge's
    // coverage is off by at most a few levels of 255.
    void add_path(Path const&, float tolerance = 0.025f);
    void add_line(FloatPoint from, FloatPoint to);

    // Calls on_row(y, x, count, coverage) for each row that has any: coverage
    // (0 to 255) for the `count` pixels from `x`, all within the clip.
    template<typename OnRow>
    void rasterize(FillRule rule, OnRow on_row)
    {
        run(rule, [](void* context, int y, int x, int count, u8 const* coverage) {
            (*static_cast<OnRow*>(context))(y, x, count, coverage);
        },
            &on_row);
    }

private:
    using RowCallback = void (*)(void*, int y, int x, int count, u8 const* coverage);

    struct Edge {
        // Downwards (y0 < y1); x relative to the clip's left.
        float x0;
        float y0;
        float y1;
        float slope; // dx / dy
        float direction;
    };

    void run(FillRule, RowCallback, void* context);
    void accumulate(float x_from, float x_to, float height);

    IntRect m_clip;
    Vector<Edge> m_edges;
    Vector<float> m_accumulation;
    Vector<u8> m_coverage;
    int m_touched_left { 0 };
    int m_touched_right { -1 };
};

}
