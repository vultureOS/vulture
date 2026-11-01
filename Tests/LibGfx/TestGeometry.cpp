/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Geometry.h>
#include <LibTest/TestCase.h>

using namespace Gfx;

namespace {

bool close(float a, float b)
{
    return Base::Math::absolute(a - b) < 1e-4f;
}

}

TEST_CASE(rectangles)
{
    IntRect a { 0, 0, 10, 10 };
    IntRect b { 5, 5, 10, 10 };
    EXPECT(a.intersected(b) == (IntRect { 5, 5, 5, 5 }));
    EXPECT(a.united(b) == (IntRect { 0, 0, 15, 15 }));
    EXPECT(a.intersects(b));
    EXPECT(!a.intersects(IntRect { 10, 0, 5, 5 }));
    EXPECT(a.intersected(IntRect { 10, 0, 5, 5 }).is_empty());
    EXPECT(a.contains(IntPoint { 9, 9 }));
    EXPECT(!a.contains(IntPoint { 10, 9 }));
    EXPECT(a.united(IntRect {}) == a);
    EXPECT((IntRect {}).united(a) == a);
    EXPECT((FloatRect { 0.5f, 0.5f, 1.0f, 1.0f }).enclosing_int_rect() == (IntRect { 0, 0, 2, 2 }));
    EXPECT((FloatRect { -0.5f, -1.5f, 1.0f, 1.0f }).enclosing_int_rect() == (IntRect { -1, -2, 2, 2 }));
}

TEST_CASE(transforms)
{
    auto point = AffineTransform::translation(2, 3).map(FloatPoint { 1, 1 });
    EXPECT(point == (FloatPoint { 3, 4 }));
    // Scaling, then moving: multiplied() applies its argument first.
    auto combined = AffineTransform::translation(10, 0).multiplied(AffineTransform::scaling(2, 2));
    EXPECT(combined.map(FloatPoint { 1, 1 }) == (FloatPoint { 12, 2 }));
    auto rotation = AffineTransform::rotation(static_cast<float>(Base::Math::pi / 2));
    auto rotated = rotation.map(FloatPoint { 1, 0 });
    EXPECT(close(rotated.x, 0) && close(rotated.y, 1));
    auto inverse = combined.multiplied(rotation).inverse();
    auto back = inverse.map(combined.multiplied(rotation).map(FloatPoint { 3, 7 }));
    EXPECT(close(back.x, 3) && close(back.y, 7));
    auto box = AffineTransform::rotation(static_cast<float>(Base::Math::pi / 2)).map(FloatRect { 0, 0, 2, 1 });
    EXPECT(close(box.x, -1) && close(box.y, 0) && close(box.width, 1) && close(box.height, 2));
    EXPECT(AffineTransform::scaling(2, 3).is_axis_aligned());
    EXPECT(!AffineTransform::rotation(1).is_axis_aligned());
    EXPECT(close(AffineTransform::scaling(2, 3).maximum_scale(), 3));
}
