/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAnimation/Curve.h>
#include <LibTest/TestCase.h>

using namespace Animation;

namespace {

bool near(float a, float b, float within = 0.001f)
{
    float difference = a - b;
    return difference > -within && difference < within;
}

}

TEST_CASE(bezier_ends_and_symmetry)
{
    CubicBezier ease(0.42f, 0, 0.58f, 1);
    EXPECT(near(ease.value_at(0), 0));
    EXPECT(near(ease.value_at(1), 1));
    EXPECT(near(ease.value_at(0.5f), 0.5f));
    // A symmetric curve mirrors around its middle.
    for (float x = 0.05f; x < 1; x += 0.05f)
        EXPECT(near(ease.value_at(x) + ease.value_at(1 - x), 1, 0.002f));
}

TEST_CASE(bezier_known_values)
{
    // CSS's "ease" at known points.
    CubicBezier ease(0.25f, 0.1f, 0.25f, 1);
    EXPECT(near(ease.value_at(0.25f), 0.4085f, 0.002f));
    EXPECT(near(ease.value_at(0.5f), 0.8024f, 0.002f));
    // A straight line is one.
    CubicBezier straight(0.25f, 0.25f, 0.75f, 0.75f);
    for (float x = 0; x <= 1; x += 0.1f)
        EXPECT(near(straight.value_at(x), x, 0.002f));
}

TEST_CASE(bezier_never_goes_backwards_in_time)
{
    // The steep ones are where Newton's method falls over.
    CubicBezier steep(0.9f, 0, 0.1f, 1);
    float last = 0;
    for (float x = 0; x <= 1; x += 0.01f) {
        float value = steep.value_at(x);
        EXPECT(value >= last - 0.0001f);
        last = value;
    }
}

TEST_CASE(spring_starts_at_rest_and_settles)
{
    Spring spring(1, 170, 26);
    EXPECT(near(spring.value_at(0), 0));
    EXPECT(near(spring.value_at(10), 1));
    float settled = spring.settling_time();
    EXPECT(settled > 0 && settled < 3);
    for (float t = settled; t < settled + 1; t += 0.01f)
        EXPECT(near(spring.value_at(t), 1, 0.0011f));
}

TEST_CASE(spring_underdamped_overshoots_and_critical_does_not)
{
    auto bouncy = Spring::with_response(0.5f, 0.5f);
    float peak = 0;
    for (float t = 0; t < 2; t += 0.001f)
        peak = max(peak, bouncy.value_at(t));
    EXPECT(peak > 1.05f);

    auto critical = Spring::with_response(0.5f, 0);
    peak = 0;
    for (float t = 0; t < 3; t += 0.001f)
        peak = max(peak, critical.value_at(t));
    EXPECT(peak <= 1.0005f);

    Spring overdamped(1, 100, 40);
    peak = 0;
    for (float t = 0; t < 5; t += 0.001f)
        peak = max(peak, overdamped.value_at(t));
    EXPECT(peak <= 1.0005f);
    EXPECT(near(overdamped.value_at(10), 1));
}

TEST_CASE(spring_initial_velocity_is_kept)
{
    Spring spring(1, 100, 20, 5);
    EXPECT(near(spring.velocity_at(0), 5, 0.2f));
    Spring underdamped(1, 400, 10, -3);
    EXPECT(near(underdamped.velocity_at(0), -3, 0.2f));
    Spring overdamped(1, 100, 40, 2);
    EXPECT(near(overdamped.velocity_at(0), 2, 0.2f));
}

TEST_CASE(timing_ends_exactly)
{
    auto timing = Timing::ease_in_out(0.25f);
    EXPECT_EQ(timing.value_at(0.25f), 1.0f);
    EXPECT_EQ(timing.value_at(5), 1.0f);
    EXPECT(near(Timing::linear(2).value_at(1), 0.5f));
    EXPECT_EQ(Timing::linear(0).value_at(0), 1.0f);
    auto spring = Timing::spring_with(0.4f, 0.2f);
    EXPECT(spring.duration > 0.2f);
    EXPECT_EQ(spring.value_at(spring.duration), 1.0f);
}
