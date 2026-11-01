/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibTest/TestCase.h>

using namespace Base::Math;

namespace {

// Within a few units in the last place: relative for ordinary values.
bool close(double value, double expected, double tolerance = 4e-16)
{
    if (is_nan(expected))
        return is_nan(value);
    if (is_infinite(expected))
        return value == expected;
    double scale = absolute(expected) < 1e-300 ? 1 : absolute(expected);
    return absolute(value - expected) <= tolerance * scale;
}

}

TEST_CASE(rounding)
{
    EXPECT_EQ(floor(2.5), 2.0);
    EXPECT_EQ(floor(-2.5), -3.0);
    EXPECT_EQ(ceil(2.5), 3.0);
    EXPECT_EQ(ceil(-2.5), -2.0);
    EXPECT_EQ(round(2.5), 3.0);
    EXPECT_EQ(round(-2.5), -3.0);
    EXPECT_EQ(round(2.4), 2.0);
    EXPECT_EQ(truncate(-2.9), -2.0);
    EXPECT_EQ(floor(1e300), 1e300);
    EXPECT(sign_bit(ceil(-0.5)));
    EXPECT_EQ(floor(2.5f), 2.0f);
    EXPECT_EQ(ceil(-2.5f), -2.0f);
    EXPECT_EQ(round(2.5f), 3.0f);
    EXPECT_EQ(round(-2.5f), -3.0f);
}

TEST_CASE(exponents_and_remainders)
{
    int exponent = 0;
    EXPECT_EQ(split_exponent(8.0, exponent), 0.5);
    EXPECT_EQ(exponent, 4);
    EXPECT_EQ(split_exponent(0.75, exponent), 0.75);
    EXPECT_EQ(exponent, 0);
    EXPECT_EQ(scale_by_power_of_two(split_exponent(5e-310, exponent), exponent), 5e-310);
    EXPECT_EQ(scale_by_power_of_two(1.0, 10), 1024.0);
    EXPECT_EQ(scale_by_power_of_two(1.0, -1074), 5e-324);
    EXPECT_EQ(modulo(7.5, 2.0), 1.5);
    EXPECT_EQ(modulo(-7.5, 2.0), -1.5);
    EXPECT_EQ(modulo(1e15 + 1, 3.0), 2.0);
    EXPECT(is_nan(modulo(1.0, 0.0)));
}

TEST_CASE(trigonometry)
{
    EXPECT(close(sine(1.0), 0.8414709848078965));
    EXPECT(close(cosine(1.0), 0.5403023058681397));
    EXPECT(close(tangent(1.0), 1.5574077246549021, 1e-15));
    EXPECT(close(sine(100.0), -0.5063656411097588, 1e-14));
    EXPECT(close(cosine(-100.0), 0.8623188722876839, 1e-14));
    EXPECT(absolute(sine(pi)) < 2e-16);
    EXPECT(absolute(cosine(pi / 2)) < 2e-16);
    EXPECT(close(cosine(pi), -1));
    for (double x = -20; x < 20; x += 0.0173) {
        double s = sine(x), c = cosine(x);
        EXPECT(absolute(s * s + c * c - 1) < 1e-14);
    }
    EXPECT(is_nan(sine(infinity())));
}

TEST_CASE(inverse_trigonometry)
{
    EXPECT(close(arc_tangent(1.0), pi / 4));
    EXPECT(close(arc_tangent(0.5), 0.4636476090008061));
    EXPECT(close(arc_tangent(-3.0), -1.2490457723982544));
    EXPECT(close(arc_tangent(1e10), 1.5707963266948965, 1e-15));
    EXPECT(close(arc_tangent2(1.0, -1.0), 3 * pi / 4));
    EXPECT(close(arc_tangent2(-1.0, -1.0), -3 * pi / 4));
    EXPECT_EQ(arc_tangent2(0.0, 1.0), 0.0);
    EXPECT(close(arc_tangent2(0.0, -1.0), pi));
    EXPECT(close(arc_tangent2(1.0, 0.0), pi / 2));
    EXPECT(close(arc_sine(0.5), pi / 6));
    EXPECT(close(arc_cosine(0.5), pi / 3));
    EXPECT(close(arc_sine(1.0), pi / 2));
    for (double x = -1.5; x < 1.5; x += 0.0071)
        EXPECT(absolute(tangent(arc_tangent(x)) - x) < 1e-14);
}

TEST_CASE(exponentials_and_logarithms)
{
    EXPECT(close(exponential(1.0), e));
    EXPECT(close(exponential(-1.0), 0.36787944117144233));
    EXPECT(close(exponential(10.0), 22026.465794806718, 1e-15));
    EXPECT_EQ(exponential(0.0), 1.0);
    EXPECT(is_infinite(exponential(1000.0)));
    EXPECT_EQ(exponential(-1000.0), 0.0);
    EXPECT(close(natural_log(e), 1));
    EXPECT(close(natural_log(10.0), ln10));
    EXPECT(close(natural_log(0.1), -2.3025850929940455));
    EXPECT(close(log2(1024.0), 10));
    EXPECT(close(log10(1000.0), 3));
    EXPECT(is_nan(natural_log(-1.0)));
    EXPECT(natural_log(0.0) == -infinity());
    for (double x = 0.01; x < 500; x *= 1.07)
        EXPECT(close(exponential(natural_log(x)), x, 1e-14));
}

TEST_CASE(powers)
{
    EXPECT_EQ(power(2.0, 10.0), 1024.0);
    EXPECT_EQ(power(2.0, -2.0), 0.25);
    EXPECT_EQ(power(-2.0, 3.0), -8.0);
    EXPECT_EQ(power(0.0, 0.0), 1.0);
    EXPECT_EQ(power(5.0, 0.0), 1.0);
    EXPECT(close(power(2.0, 0.5), 1.4142135623730951));
    EXPECT(close(power(10.0, 2.5), 316.22776601683796, 1e-15));
    EXPECT(is_nan(power(-2.0, 0.5)));
    EXPECT(close(cube_root(27.0), 3));
    EXPECT(close(cube_root(-8.0), -2));
    EXPECT(close(hypotenuse(3.0, 4.0), 5));
    EXPECT(close(hypotenuse(3e300, 4e300), 5e300));
    EXPECT(close(square_root(2.0), 1.4142135623730951));
    EXPECT(close(hyperbolic_sine(1.0), 1.1752011936438014));
    EXPECT(close(hyperbolic_cosine(1.0), 1.5430806348152437));
    EXPECT(close(hyperbolic_tangent(1.0), 0.7615941559557649));
}
