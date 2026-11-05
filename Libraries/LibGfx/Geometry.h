/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Math.h>
#include <LibBase/StdLibExtras.h>
#include <LibBase/Types.h>

namespace Gfx {

template<typename T>
struct Point {
    T x {};
    T y {};

    constexpr Point operator+(Point other) const { return { x + other.x, y + other.y }; }
    constexpr Point operator-(Point other) const { return { x - other.x, y - other.y }; }
    constexpr Point operator*(T factor) const { return { x * factor, y * factor }; }
    constexpr bool operator==(Point const&) const = default;

    template<typename U>
    [[nodiscard]] constexpr Point<U> to() const { return { static_cast<U>(x), static_cast<U>(y) }; }
};

template<typename T>
struct Size {
    T width {};
    T height {};

    [[nodiscard]] constexpr bool is_empty() const { return width <= 0 || height <= 0; }
    constexpr bool operator==(Size const&) const = default;

    template<typename U>
    [[nodiscard]] constexpr Size<U> to() const { return { static_cast<U>(width), static_cast<U>(height) }; }
};

template<typename T>
struct Rect {
    T x {};
    T y {};
    T width {};
    T height {};

    [[nodiscard]] static constexpr Rect from_edges(T left, T top, T right, T bottom) { return { left, top, right - left, bottom - top }; }

    [[nodiscard]] constexpr T left() const { return x; }
    [[nodiscard]] constexpr T top() const { return y; }
    // Just past the last column and row.
    [[nodiscard]] constexpr T right() const { return x + width; }
    [[nodiscard]] constexpr Point<T> center() const { return { x + width / 2, y + height / 2 }; }
    [[nodiscard]] constexpr T bottom() const { return y + height; }
    [[nodiscard]] constexpr Point<T> origin() const { return { x, y }; }
    [[nodiscard]] constexpr Size<T> size() const { return { width, height }; }
    [[nodiscard]] constexpr bool is_empty() const { return width <= 0 || height <= 0; }

    [[nodiscard]] constexpr bool contains(Point<T> point) const
    {
        return point.x >= x && point.y >= y && point.x < right() && point.y < bottom();
    }

    [[nodiscard]] constexpr bool contains(Rect const& other) const
    {
        return other.x >= x && other.y >= y && other.right() <= right() && other.bottom() <= bottom();
    }

    [[nodiscard]] constexpr bool intersects(Rect const& other) const
    {
        return !is_empty() && !other.is_empty() && other.x < right() && x < other.right() && other.y < bottom() && y < other.bottom();
    }

    // Empty (all zero) if they do not meet.
    [[nodiscard]] constexpr Rect intersected(Rect const& other) const
    {
        T new_left = max(x, other.x);
        T new_top = max(y, other.y);
        T new_right = min(right(), other.right());
        T new_bottom = min(bottom(), other.bottom());
        if (new_left >= new_right || new_top >= new_bottom)
            return {};
        return from_edges(new_left, new_top, new_right, new_bottom);
    }

    // The smallest rectangle holding both; an empty one adds nothing.
    [[nodiscard]] constexpr Rect united(Rect const& other) const
    {
        if (is_empty())
            return other;
        if (other.is_empty())
            return *this;
        return from_edges(min(x, other.x), min(y, other.y), max(right(), other.right()), max(bottom(), other.bottom()));
    }

    [[nodiscard]] constexpr Rect translated(T dx, T dy) const { return { x + dx, y + dy, width, height }; }
    [[nodiscard]] constexpr Rect inflated(T by) const { return { x - by, y - by, width + 2 * by, height + 2 * by }; }
    [[nodiscard]] constexpr bool operator==(Rect const&) const = default;

    template<typename U>
    [[nodiscard]] constexpr Rect<U> to() const { return { static_cast<U>(x), static_cast<U>(y), static_cast<U>(width), static_cast<U>(height) }; }

    // The smallest whole-pixel rectangle holding this one.
    [[nodiscard]] Rect<int> enclosing_int_rect() const
    {
        int new_left = static_cast<int>(Base::Math::floor(static_cast<double>(x)));
        int new_top = static_cast<int>(Base::Math::floor(static_cast<double>(y)));
        int new_right = static_cast<int>(Base::Math::ceil(static_cast<double>(right())));
        int new_bottom = static_cast<int>(Base::Math::ceil(static_cast<double>(bottom())));
        return { new_left, new_top, new_right - new_left, new_bottom - new_top };
    }
};

using IntPoint = Point<int>;
using FloatPoint = Point<float>;
using IntSize = Size<int>;
using FloatSize = Size<float>;
using IntRect = Rect<int>;
using FloatRect = Rect<float>;

// A 2D affine map: x' = a x + c y + e, y' = b x + d y + f.
class AffineTransform {
public:
    constexpr AffineTransform() = default;
    constexpr AffineTransform(float a, float b, float c, float d, float e, float f)
        : m_a(a)
        , m_b(b)
        , m_c(c)
        , m_d(d)
        , m_e(e)
        , m_f(f)
    {
    }

    [[nodiscard]] static constexpr AffineTransform translation(float x, float y) { return { 1, 0, 0, 1, x, y }; }
    [[nodiscard]] static constexpr AffineTransform scaling(float x, float y) { return { x, 0, 0, y, 0, 0 }; }
    [[nodiscard]] static AffineTransform rotation(float radians)
    {
        auto s = static_cast<float>(Base::Math::sine(radians));
        auto c = static_cast<float>(Base::Math::cosine(radians));
        return { c, s, -s, c, 0, 0 };
    }

    [[nodiscard]] constexpr float a() const { return m_a; }
    [[nodiscard]] constexpr float b() const { return m_b; }
    [[nodiscard]] constexpr float c() const { return m_c; }
    [[nodiscard]] constexpr float d() const { return m_d; }
    [[nodiscard]] constexpr float e() const { return m_e; }
    [[nodiscard]] constexpr float f() const { return m_f; }

    [[nodiscard]] constexpr FloatPoint map(FloatPoint point) const
    {
        return { m_a * point.x + m_c * point.y + m_e, m_b * point.x + m_d * point.y + m_f };
    }

    // The bounding box of the mapped rectangle.
    [[nodiscard]] constexpr FloatRect map(FloatRect const& rect) const
    {
        FloatPoint corners[4] = { map(rect.origin()), map(FloatPoint { rect.right(), rect.y }), map(FloatPoint { rect.x, rect.bottom() }), map(FloatPoint { rect.right(), rect.bottom() }) };
        float left = corners[0].x, right = corners[0].x, top = corners[0].y, bottom = corners[0].y;
        for (auto const& corner : corners) {
            left = min(left, corner.x);
            right = max(right, corner.x);
            top = min(top, corner.y);
            bottom = max(bottom, corner.y);
        }
        return FloatRect::from_edges(left, top, right, bottom);
    }

    // This map after `other`: first `other`, then this.
    [[nodiscard]] constexpr AffineTransform multiplied(AffineTransform const& other) const
    {
        return {
            m_a * other.m_a + m_c * other.m_b,
            m_b * other.m_a + m_d * other.m_b,
            m_a * other.m_c + m_c * other.m_d,
            m_b * other.m_c + m_d * other.m_d,
            m_a * other.m_e + m_c * other.m_f + m_e,
            m_b * other.m_e + m_d * other.m_f + m_f,
        };
    }

    [[nodiscard]] constexpr float determinant() const { return m_a * m_d - m_b * m_c; }
    [[nodiscard]] constexpr bool is_invertible() const { return determinant() != 0; }

    // Undefined if it is not invertible.
    [[nodiscard]] constexpr AffineTransform inverse() const
    {
        float factor = 1 / determinant();
        return {
            m_d * factor,
            -m_b * factor,
            -m_c * factor,
            m_a * factor,
            (m_c * m_f - m_d * m_e) * factor,
            (m_b * m_e - m_a * m_f) * factor,
        };
    }

    // Only moves and scales along the axes: rectangles stay rectangles.
    [[nodiscard]] constexpr bool is_axis_aligned() const { return m_b == 0 && m_c == 0; }
    [[nodiscard]] constexpr bool is_translation() const { return m_a == 1 && m_d == 1 && m_b == 0 && m_c == 0; }

    // How much it stretches lengths, at most.
    [[nodiscard]] float maximum_scale() const
    {
        float first = static_cast<float>(Base::Math::square_root(static_cast<double>(m_a * m_a + m_b * m_b)));
        float second = static_cast<float>(Base::Math::square_root(static_cast<double>(m_c * m_c + m_d * m_d)));
        return max(first, second);
    }

    constexpr bool operator==(AffineTransform const&) const = default;

private:
    float m_a { 1 };
    float m_b { 0 };
    float m_c { 0 };
    float m_d { 1 };
    float m_e { 0 };
    float m_f { 0 };
};

}
