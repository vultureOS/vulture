/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Path.h>

namespace Gfx {

namespace Math = Base::Math;

void Path::move_to(FloatPoint point)
{
    m_commands.append(Command::Move);
    m_points.append(point);
    m_subpath_start = point;
    m_current = point;
    m_has_current = true;
    m_needs_move = false;
}

void Path::line_to(FloatPoint point)
{
    if (!m_has_current) {
        move_to(point);
        return;
    }
    if (m_needs_move)
        move_to(m_subpath_start);
    m_commands.append(Command::Line);
    m_points.append(point);
    m_current = point;
}

void Path::quad_to(FloatPoint control, FloatPoint end)
{
    if (!m_has_current)
        move_to(control);
    if (m_needs_move)
        move_to(m_subpath_start);
    m_commands.append(Command::Quad);
    m_points.append(control);
    m_points.append(end);
    m_current = end;
}

void Path::cubic_to(FloatPoint control1, FloatPoint control2, FloatPoint end)
{
    if (!m_has_current)
        move_to(control1);
    if (m_needs_move)
        move_to(m_subpath_start);
    m_commands.append(Command::Cubic);
    m_points.append(control1);
    m_points.append(control2);
    m_points.append(end);
    m_current = end;
}

void Path::arc_to(FloatPoint center, float radius, float start_radians, float end_radians)
{
    auto point_at = [&](double angle) {
        return FloatPoint { center.x + radius * static_cast<float>(Math::cosine(angle)), center.y + radius * static_cast<float>(Math::sine(angle)) };
    };
    double sweep = static_cast<double>(end_radians - start_radians);
    auto start = point_at(start_radians);
    if (m_has_current && !m_needs_move)
        line_to(start);
    else
        move_to(start);
    // Quarter turns at most: a cubic fits those to within 0.03% of the radius.
    int segments = max(1, static_cast<int>(Math::ceil(Math::absolute(sweep) / (Math::pi / 2) - 1e-6)));
    double step = sweep / segments;
    double handle = 4.0 / 3.0 * Math::tangent(step / 4);
    double angle = start_radians;
    for (int i = 0; i < segments; ++i) {
        double next = angle + step;
        auto from = point_at(angle);
        auto to = point_at(next);
        auto extent = static_cast<float>(handle) * radius;
        FloatPoint control1 { from.x - extent * static_cast<float>(Math::sine(angle)), from.y + extent * static_cast<float>(Math::cosine(angle)) };
        FloatPoint control2 { to.x + extent * static_cast<float>(Math::sine(next)), to.y - extent * static_cast<float>(Math::cosine(next)) };
        cubic_to(control1, control2, to);
        angle = next;
    }
}

void Path::close()
{
    if (!m_has_current || m_needs_move)
        return;
    m_commands.append(Command::Close);
    m_current = m_subpath_start;
    // More drawing starts a new subpath there.
    m_needs_move = true;
}

void Path::add_rect(FloatRect const& rect)
{
    move_to(rect.origin());
    line_to({ rect.right(), rect.y });
    line_to({ rect.right(), rect.bottom() });
    line_to({ rect.x, rect.bottom() });
    close();
}

void Path::add_rounded_rect(FloatRect const& rect, float radius)
{
    radius = min(radius, min(rect.width, rect.height) / 2);
    if (radius <= 0) {
        add_rect(rect);
        return;
    }
    // A quarter circle as one cubic.
    constexpr float kappa = 0.5522847498f;
    float handle = radius * (1 - kappa);
    float left = rect.x, top = rect.y, right = rect.right(), bottom = rect.bottom();
    move_to({ left + radius, top });
    line_to({ right - radius, top });
    cubic_to({ right - handle, top }, { right, top + handle }, { right, top + radius });
    line_to({ right, bottom - radius });
    cubic_to({ right, bottom - handle }, { right - handle, bottom }, { right - radius, bottom });
    line_to({ left + radius, bottom });
    cubic_to({ left + handle, bottom }, { left, bottom - handle }, { left, bottom - radius });
    line_to({ left, top + radius });
    cubic_to({ left, top + handle }, { left + handle, top }, { left + radius, top });
    close();
}

void Path::add_continuous_rounded_rect(FloatRect const& rect, float radius)
{
    // The extent of the curve along an edge, in radii.
    constexpr float extent = 1.52866483f;
    radius = min(radius, min(rect.width, rect.height) / (2 * extent));
    if (radius <= 0) {
        add_rect(rect);
        return;
    }
    float left = rect.x, top = rect.y, right = rect.right(), bottom = rect.bottom();
    // One corner's curve in radii from the corner, along the edge coming in
    // (`along`) and the edge going out (`across`).
    struct Knot {
        float along;
        float across;
    };
    constexpr Knot knots[] = {
        { 1.08849323f, 0 }, { 0.86840689f, 0 }, { 0.63149399f, 0.07491100f },
        { 0.37282392f, 0.16905899f }, { 0.16906013f, 0.37282401f }, { 0.07491176f, 0.63149399f },
        { 0, 0.86840701f }, { 0, 1.08849299f }, { 0, extent },
    };
    auto corner = [&](auto map) {
        for (size_t i = 0; i < array_size(knots); i += 3)
            cubic_to(map(knots[i]), map(knots[i + 1]), map(knots[i + 2]));
    };
    move_to({ left + extent * radius, top });
    line_to({ right - extent * radius, top });
    corner([&](Knot knot) { return FloatPoint { right - knot.along * radius, top + knot.across * radius }; });
    line_to({ right, bottom - extent * radius });
    corner([&](Knot knot) { return FloatPoint { right - knot.across * radius, bottom - knot.along * radius }; });
    line_to({ left + extent * radius, bottom });
    corner([&](Knot knot) { return FloatPoint { left + knot.along * radius, bottom - knot.across * radius }; });
    line_to({ left, top + extent * radius });
    corner([&](Knot knot) { return FloatPoint { left + knot.across * radius, top + knot.along * radius }; });
    close();
}

void Path::add_ellipse(FloatRect const& rect)
{
    constexpr float kappa = 0.5522847498f;
    float rx = rect.width / 2, ry = rect.height / 2;
    float cx = rect.x + rx, cy = rect.y + ry;
    float hx = rx * kappa, hy = ry * kappa;
    move_to({ cx + rx, cy });
    cubic_to({ cx + rx, cy + hy }, { cx + hx, cy + ry }, { cx, cy + ry });
    cubic_to({ cx - hx, cy + ry }, { cx - rx, cy + hy }, { cx - rx, cy });
    cubic_to({ cx - rx, cy - hy }, { cx - hx, cy - ry }, { cx, cy - ry });
    cubic_to({ cx + hx, cy - ry }, { cx + rx, cy - hy }, { cx + rx, cy });
    close();
}

void Path::add_circle(FloatPoint center, float radius)
{
    add_ellipse({ center.x - radius, center.y - radius, radius * 2, radius * 2 });
}

FloatRect Path::bounds() const
{
    if (m_points.is_empty())
        return {};
    float left = m_points[0].x, right = m_points[0].x, top = m_points[0].y, bottom = m_points[0].y;
    for (auto const& point : m_points) {
        left = min(left, point.x);
        right = max(right, point.x);
        top = min(top, point.y);
        bottom = max(bottom, point.y);
    }
    return FloatRect::from_edges(left, top, right, bottom);
}

Path Path::transformed(AffineTransform const& transform) const
{
    Path result = *this;
    for (auto& point : result.m_points)
        point = transform.map(point);
    result.m_subpath_start = transform.map(m_subpath_start);
    result.m_current = transform.map(m_current);
    return result;
}

FloatPoint Path::quad_at(FloatPoint from, FloatPoint control, FloatPoint to, float t)
{
    float u = 1 - t;
    return {
        u * u * from.x + 2 * u * t * control.x + t * t * to.x,
        u * u * from.y + 2 * u * t * control.y + t * t * to.y,
    };
}

FloatPoint Path::cubic_at(FloatPoint from, FloatPoint control1, FloatPoint control2, FloatPoint to, float t)
{
    float u = 1 - t;
    float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    return {
        a * from.x + b * control1.x + c * control2.x + d * to.x,
        a * from.y + b * control1.y + c * control2.y + d * to.y,
    };
}

// How many equal steps in the parameter keep the lines within `tolerance`
// (Wang's formula), from how far the control polygon bends.
int Path::quad_steps(FloatPoint from, FloatPoint control, FloatPoint to, float tolerance)
{
    double dx = static_cast<double>(from.x - 2 * control.x + to.x);
    double dy = static_cast<double>(from.y - 2 * control.y + to.y);
    double bend = Math::square_root(dx * dx + dy * dy);
    double steps = Math::square_root(bend / (4 * static_cast<double>(tolerance)));
    return clamp(static_cast<int>(Math::ceil(steps)), 1, 512);
}

int Path::cubic_steps(FloatPoint from, FloatPoint control1, FloatPoint control2, FloatPoint to, float tolerance)
{
    auto bend = [](FloatPoint a, FloatPoint b, FloatPoint c) {
        double dx = static_cast<double>(a.x - 2 * b.x + c.x);
        double dy = static_cast<double>(a.y - 2 * b.y + c.y);
        return dx * dx + dy * dy;
    };
    double most = Math::square_root(max(bend(from, control1, control2), bend(control1, control2, to)));
    double steps = Math::square_root(3 * most / (4 * static_cast<double>(tolerance)));
    return clamp(static_cast<int>(Math::ceil(steps)), 1, 512);
}

}
