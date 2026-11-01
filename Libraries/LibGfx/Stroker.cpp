/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Stroker.h>

namespace Gfx {

namespace Math = Base::Math;

namespace {

struct Polyline {
    Vector<FloatPoint> points;
    bool closed { false };
};

FloatPoint normal_of(FloatPoint from, FloatPoint to)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    auto length = static_cast<float>(Math::square_root(static_cast<double>(dx * dx + dy * dy)));
    return { dy / length, -dx / length };
}

class Outline {
public:
    Outline(Path& path, StrokeStyle const& style, float tolerance)
        : m_path(path)
        , m_style(style)
        , m_half_width(style.width / 2)
    {
        // Enough points on a round cap or join to stay within the tolerance.
        double ratio = 1 - static_cast<double>(min(tolerance / max(m_half_width, 0.001f), 1.0f));
        m_round_step = static_cast<float>(2 * Math::arc_cosine(ratio));
        if (m_round_step < 0.02f)
            m_round_step = 0.02f;
    }

    void add(Polyline const& line)
    {
        // Without repeated points, as a segment of no length has no direction.
        Vector<FloatPoint> points;
        for (auto const& point : line.points) {
            if (points.is_empty() || point != points.last())
                points.append(point);
        }
        bool closed = line.closed;
        if (closed && points.size() > 1 && points.first() == points.last())
            points.take_last();
        if (points.size() < 2) {
            if (!points.is_empty())
                add_dot(points[0], closed);
            return;
        }
        if (closed && points.size() > 2) {
            // Two rings, opposite ways around: the stroke is between them.
            add_side(points, true, true);
            m_path.close();
            Vector<FloatPoint> reversed;
            for (size_t i = points.size(); i > 0; --i)
                reversed.append(points[i - 1]);
            add_side(reversed, true, true);
            m_path.close();
            return;
        }
        add_side(points, false, true);
        add_cap(points.last(), points[points.size() - 2]);
        Vector<FloatPoint> reversed;
        for (size_t i = points.size(); i > 0; --i)
            reversed.append(points[i - 1]);
        add_side(reversed, false, false);
        add_cap(points[0], points[1]);
        m_path.close();
    }

private:
    void emit(FloatPoint point, bool& started)
    {
        if (started) {
            m_path.line_to(point);
        } else {
            m_path.move_to(point);
            started = true;
        }
    }

    // A zero-length subpath shows only as what its cap draws.
    void add_dot(FloatPoint point, bool closed)
    {
        (void)closed;
        if (m_style.cap == LineCap::Round) {
            m_path.add_circle(point, m_half_width);
        } else if (m_style.cap == LineCap::Square) {
            m_path.add_rect({ point.x - m_half_width, point.y - m_half_width, m_style.width, m_style.width });
        }
    }

    void add_arc_points(FloatPoint center, FloatPoint from_normal, FloatPoint to_normal, bool& started)
    {
        double from_angle = Math::arc_tangent2(static_cast<double>(from_normal.y), static_cast<double>(from_normal.x));
        double to_angle = Math::arc_tangent2(static_cast<double>(to_normal.y), static_cast<double>(to_normal.x));
        // The way this outline turns: toward decreasing... whichever is under half a turn.
        double sweep = to_angle - from_angle;
        while (sweep > Math::pi)
            sweep -= 2 * Math::pi;
        while (sweep < -Math::pi)
            sweep += 2 * Math::pi;
        int steps = max(1, static_cast<int>(Math::ceil(Math::absolute(sweep) / static_cast<double>(m_round_step))));
        for (int i = 1; i < steps; ++i) {
            double angle = from_angle + sweep * i / steps;
            emit({ center.x + m_half_width * static_cast<float>(Math::cosine(angle)), center.y + m_half_width * static_cast<float>(Math::sine(angle)) }, started);
        }
    }

    // Along one side, offset to the normal's direction. Inner corners go
    // through the center point, so the winding stays one way all around.
    void add_side(Vector<FloatPoint> const& points, bool closed, bool starting)
    {
        size_t count = points.size();
        size_t segments = closed ? count : count - 1;
        bool started = !starting;
        auto offset = [&](FloatPoint point, FloatPoint normal) {
            return FloatPoint { point.x + normal.x * m_half_width, point.y + normal.y * m_half_width };
        };
        if (closed)
            started = false;
        FloatPoint first_normal = normal_of(points[0], points[1]);
        emit(offset(points[0], first_normal), started);
        auto length_between = [](FloatPoint a, FloatPoint b) {
            float dx = b.x - a.x, dy = b.y - a.y;
            return static_cast<float>(Math::square_root(static_cast<double>(dx * dx + dy * dy)));
        };
        for (size_t i = 0; i < segments; ++i) {
            auto const& from = points[i];
            auto const& to = points[(i + 1) % count];
            FloatPoint normal = normal_of(from, to);
            // The line ends here: no join.
            if (!closed && i + 1 == segments) {
                emit(offset(to, normal), started);
                break;
            }
            auto const& after = points[(i + 2) % count];
            FloatPoint next_normal = normal_of(to, after);
            float cross = normal.x * next_normal.y - normal.y * next_normal.x;
            float dot = normal.x * next_normal.x + normal.y * next_normal.y;
            // Straight on: nothing to join.
            if (cross == 0 && dot > 0) {
                emit(offset(to, normal), started);
                continue;
            }
            // Where the offset lines of the two segments meet: the miter tip,
            // on the outside of a corner; on the inside, the corner itself.
            FloatPoint tip {};
            float denominator = 1 + dot;
            if (denominator > 0)
                tip = { to.x + (normal.x + next_normal.x) / denominator * m_half_width, to.y + (normal.y + next_normal.y) / denominator * m_half_width };
            if (cross < 0) {
                // Turning toward this side. If both segments reach that far,
                // that one point is the outline; a short segment does not, so
                // go through the center, which keeps the winding one way.
                float reach = denominator > 0 ? m_half_width * Math::absolute(cross) / denominator : 0;
                if (denominator > 0 && reach <= min(length_between(from, to), length_between(to, after))) {
                    emit(tip, started);
                } else {
                    emit(offset(to, normal), started);
                    emit(to, started);
                    emit(offset(to, next_normal), started);
                }
                continue;
            }
            emit(offset(to, normal), started);
            switch (m_style.join) {
            case LineJoin::Round:
                add_arc_points(to, normal, next_normal, started);
                break;
            case LineJoin::Miter:
                // Miter length / width = sqrt(2 / (1 + dot)).
                if (denominator > 0 && 2 / denominator <= m_style.miter_limit * m_style.miter_limit)
                    emit(tip, started);
                break;
            case LineJoin::Bevel:
                break;
            }
            emit(offset(to, next_normal), started);
        }
    }

    // Around the end `point` of a segment arriving from `before`.
    void add_cap(FloatPoint point, FloatPoint before)
    {
        FloatPoint normal = normal_of(before, point);
        FloatPoint forward { -normal.y, normal.x };
        bool started = true;
        switch (m_style.cap) {
        case LineCap::Butt:
            break;
        case LineCap::Square:
            emit({ point.x + (normal.x + forward.x) * m_half_width, point.y + (normal.y + forward.y) * m_half_width }, started);
            emit({ point.x + (forward.x - normal.x) * m_half_width, point.y + (forward.y - normal.y) * m_half_width }, started);
            break;
        case LineCap::Round: {
            double from_angle = Math::arc_tangent2(static_cast<double>(normal.y), static_cast<double>(normal.x));
            int steps = max(2, static_cast<int>(Math::ceil(Math::pi / static_cast<double>(m_round_step))));
            for (int i = 1; i < steps; ++i) {
                double angle = from_angle + Math::pi * i / steps;
                emit({ point.x + m_half_width * static_cast<float>(Math::cosine(angle)), point.y + m_half_width * static_cast<float>(Math::sine(angle)) }, started);
            }
            break;
        }
        }
    }

    Path& m_path;
    StrokeStyle const& m_style;
    float m_half_width;
    float m_round_step;
};

// Cuts lines into their dashes.
Vector<Polyline> dashed(Vector<Polyline> const& lines, StrokeStyle const& style)
{
    float total = 0;
    for (float length : style.dashes)
        total += length;
    if (total <= 0)
        return lines;
    Vector<Polyline> result;
    for (auto const& line : lines) {
        size_t index = 0;
        auto phase = static_cast<float>(Math::modulo(static_cast<double>(style.dash_phase), static_cast<double>(total)));
        if (phase < 0)
            phase += total;
        while (phase >= style.dashes[index]) {
            phase -= style.dashes[index];
            index = (index + 1) % style.dashes.size();
        }
        float left = style.dashes[index] - phase;
        bool drawing = index % 2 == 0;
        Polyline current;
        size_t count = line.points.size();
        size_t segments = line.closed ? count : count - 1;
        if (count < 2)
            continue;
        if (drawing)
            current.points.append(line.points[0]);
        for (size_t i = 0; i < segments; ++i) {
            auto from = line.points[i];
            auto to = line.points[(i + 1) % count];
            float dx = to.x - from.x, dy = to.y - from.y;
            auto length = static_cast<float>(Math::square_root(static_cast<double>(dx * dx + dy * dy)));
            float travelled = 0;
            while (length - travelled > left) {
                travelled += left;
                FloatPoint at { from.x + dx * travelled / length, from.y + dy * travelled / length };
                if (drawing) {
                    current.points.append(at);
                    result.append(move(current));
                    current = {};
                } else {
                    current.points.append(at);
                }
                drawing = !drawing;
                index = (index + 1) % style.dashes.size();
                left = style.dashes[index];
                // An odd list repeats with on and off swapped; zero lengths must end.
                if (left <= 0)
                    left = 0.0001f;
            }
            left -= length - travelled;
            if (drawing)
                current.points.append(to);
        }
        if (drawing && current.points.size() > 0)
            result.append(move(current));
    }
    return result;
}

}

Path stroked(Path const& path, StrokeStyle const& style, float tolerance)
{
    Vector<Polyline> lines;
    path.flatten(tolerance, [&](FloatPoint point, bool starts_subpath) {
        if (starts_subpath || lines.is_empty())
            lines.append(Polyline {});
        lines.last().points.append(point); }, [&] {
        if (!lines.is_empty())
            lines.last().closed = true; });
    Path result;
    if (style.width <= 0)
        return result;
    if (!style.dashes.is_empty())
        lines = dashed(lines, style);
    Outline outline(result, style, tolerance);
    for (auto const& line : lines) {
        if (!line.points.is_empty())
            outline.add(line);
    }
    return result;
}

}
