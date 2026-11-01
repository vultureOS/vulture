/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Geometry.h>

namespace Gfx {

struct GradientStop {
    float offset;
    Color color;
};

// Colors along a line, or out from a center, interpolated between premultiplied
// stops. Beyond the ends the end colors carry on.
class Gradient {
public:
    [[nodiscard]] static Gradient linear(FloatPoint from, FloatPoint to, Span<GradientStop const> stops);
    [[nodiscard]] static Gradient radial(FloatPoint center, float radius, Span<GradientStop const> stops);

    [[nodiscard]] bool is_radial() const { return m_radial; }
    [[nodiscard]] FloatPoint from() const { return m_from; }
    [[nodiscard]] FloatPoint to() const { return m_to; }
    [[nodiscard]] float radius() const { return m_radius; }

    // At t, 0 at the start and 1 at the end.
    [[nodiscard]] Pixel pixel_at(float t) const
    {
        if (!(t > 0))
            return m_table[0];
        if (t >= 1)
            return m_table[table_size - 1];
        return m_table[static_cast<size_t>(t * (table_size - 1) + 0.5f)];
    }

private:
    static constexpr size_t table_size = 1024;

    void build_table(Span<GradientStop const>);

    bool m_radial { false };
    FloatPoint m_from;
    FloatPoint m_to;
    float m_radius { 0 };
    Vector<Pixel> m_table;
};

}
