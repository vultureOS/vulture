/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/Vector.h>
#include <LibGfx/Geometry.h>

namespace Gfx {

// Lines and Bézier curves, in any number of subpaths.
class Path {
public:
    void move_to(FloatPoint);
    void line_to(FloatPoint);
    void quad_to(FloatPoint control, FloatPoint end);
    void cubic_to(FloatPoint control1, FloatPoint control2, FloatPoint end);
    // From `start_radians` to `end_radians` around a circle, clockwise on
    // screen when end > start, joined to the path by a line (or starting a
    // subpath, if there is none).
    void arc_to(FloatPoint center, float radius, float start_radians, float end_radians);
    void close();

    void add_rect(FloatRect const&);
    void add_rounded_rect(FloatRect const&, float radius);
    // With macOS's continuous corners: the curvature eases in rather than
    // starting at once, so the corner takes about 1.53 radii of each edge.
    void add_continuous_rounded_rect(FloatRect const&, float radius);
    void add_ellipse(FloatRect const&);
    void add_circle(FloatPoint center, float radius);

    [[nodiscard]] bool is_empty() const { return m_commands.is_empty(); }
    // Of its points, control points included.
    [[nodiscard]] FloatRect bounds() const;
    [[nodiscard]] Path transformed(AffineTransform const&) const;

    // As straight lines, no further than `tolerance` from the curves. Each
    // subpath in turn: on_point(point, starts_subpath); on_close() if it
    // ended with close().
    template<typename OnPoint, typename OnClose>
    void flatten(float tolerance, OnPoint on_point, OnClose on_close) const
    {
        FloatPoint current;
        size_t next_point = 0;
        for (auto command : m_commands) {
            switch (command) {
            case Command::Move:
                current = m_points[next_point++];
                on_point(current, true);
                break;
            case Command::Line:
                current = m_points[next_point++];
                on_point(current, false);
                break;
            case Command::Quad: {
                auto control = m_points[next_point++];
                auto end = m_points[next_point++];
                int steps = quad_steps(current, control, end, tolerance);
                for (int i = 1; i <= steps; ++i)
                    on_point(i == steps ? end : quad_at(current, control, end, static_cast<float>(i) / static_cast<float>(steps)), false);
                current = end;
                break;
            }
            case Command::Cubic: {
                auto control1 = m_points[next_point++];
                auto control2 = m_points[next_point++];
                auto end = m_points[next_point++];
                int steps = cubic_steps(current, control1, control2, end, tolerance);
                for (int i = 1; i <= steps; ++i)
                    on_point(i == steps ? end : cubic_at(current, control1, control2, end, static_cast<float>(i) / static_cast<float>(steps)), false);
                current = end;
                break;
            }
            case Command::Close:
                on_close();
                break;
            }
        }
    }

private:
    enum class Command : u8 {
        Move,
        Line,
        Quad,
        Cubic,
        Close,
    };

    static FloatPoint quad_at(FloatPoint from, FloatPoint control, FloatPoint to, float t);
    static FloatPoint cubic_at(FloatPoint from, FloatPoint control1, FloatPoint control2, FloatPoint to, float t);
    static int quad_steps(FloatPoint from, FloatPoint control, FloatPoint to, float tolerance);
    static int cubic_steps(FloatPoint from, FloatPoint control1, FloatPoint control2, FloatPoint to, float tolerance);

    Vector<Command> m_commands;
    Vector<FloatPoint> m_points;
    FloatPoint m_subpath_start;
    FloatPoint m_current;
    bool m_has_current { false };
    bool m_needs_move { false };
};

}
