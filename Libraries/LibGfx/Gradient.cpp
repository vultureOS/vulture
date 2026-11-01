/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibGfx/Gradient.h>

namespace Gfx {

Gradient Gradient::linear(FloatPoint from, FloatPoint to, Span<GradientStop const> stops)
{
    Gradient gradient;
    gradient.m_from = from;
    gradient.m_to = to;
    gradient.build_table(stops);
    return gradient;
}

Gradient Gradient::radial(FloatPoint center, float radius, Span<GradientStop const> stops)
{
    Gradient gradient;
    gradient.m_radial = true;
    gradient.m_from = center;
    gradient.m_radius = radius;
    gradient.build_table(stops);
    return gradient;
}

void Gradient::build_table(Span<GradientStop const> stops)
{
    MUST(m_table.try_resize(table_size));
    if (stops.size() == 0) {
        __builtin_memset(m_table.data(), 0, table_size * sizeof(Pixel));
        return;
    }
    size_t next = 0;
    for (size_t i = 0; i < table_size; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(table_size - 1);
        while (next < stops.size() && stops[next].offset <= t)
            ++next;
        if (next == 0) {
            m_table[i] = stops[0].color.to_pixel();
        } else if (next == stops.size()) {
            m_table[i] = stops[stops.size() - 1].color.to_pixel();
        } else {
            auto const& before = stops[next - 1];
            auto const& after = stops[next];
            float span = after.offset - before.offset;
            float mix = span > 0 ? (t - before.offset) / span : 1;
            Pixel a = before.color.to_pixel();
            Pixel b = after.color.to_pixel();
            auto lerp = [mix](u8 x, u8 y) {
                return static_cast<u8>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * mix + 0.5f);
            };
            m_table[i] = make_pixel(lerp(pixel_red(a), pixel_red(b)), lerp(pixel_green(a), pixel_green(b)), lerp(pixel_blue(a), pixel_blue(b)), lerp(pixel_alpha(a), pixel_alpha(b)));
        }
    }
}

}
