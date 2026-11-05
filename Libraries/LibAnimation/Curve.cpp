/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAnimation/Curve.h>
#include <LibBase/Math.h>
#include <LibBase/StdLibExtras.h>

namespace Animation {

namespace Math = Base::Math;

namespace {

float bezier_axis(float a, float b, float t)
{
    // With end points 0 and 1.
    float inverse = 1 - t;
    return 3 * inverse * inverse * t * a + 3 * inverse * t * t * b + t * t * t;
}

float bezier_slope(float a, float b, float t)
{
    float inverse = 1 - t;
    return 3 * inverse * inverse * a + 6 * inverse * t * (b - a) + 3 * t * t * (1 - b);
}

float exp_f(float x) { return static_cast<float>(Math::exponential(static_cast<double>(x))); }
float sin_f(float x) { return static_cast<float>(Math::sine(static_cast<double>(x))); }
float cos_f(float x) { return static_cast<float>(Math::cosine(static_cast<double>(x))); }
float sqrt_f(float x) { return static_cast<float>(Math::square_root(static_cast<double>(x))); }

constexpr float pi = 3.14159265f;

}

float CubicBezier::value_at(float x) const
{
    if (!(x > 0))
        return 0;
    if (x >= 1)
        return 1;
    // Newton's method for the t at this x, falling back on bisection where the
    // slope is too flat to trust.
    float t = x;
    for (int i = 0; i < 8; ++i) {
        float error = bezier_axis(m_x1, m_x2, t) - x;
        if (error > -0.00001f && error < 0.00001f)
            return bezier_axis(m_y1, m_y2, t);
        float slope = bezier_slope(m_x1, m_x2, t);
        if (slope > -0.000001f && slope < 0.000001f)
            break;
        t -= error / slope;
    }
    float low = 0, high = 1;
    t = x;
    for (int i = 0; i < 32; ++i) {
        float found = bezier_axis(m_x1, m_x2, t);
        if (found > x - 0.00001f && found < x + 0.00001f)
            break;
        if (found < x)
            low = t;
        else
            high = t;
        t = (low + high) / 2;
    }
    return bezier_axis(m_y1, m_y2, t);
}

Spring::Spring(float mass, float stiffness, float damping, float initial_velocity)
    : m_natural(sqrt_f(stiffness / mass))
    , m_ratio(damping / (2 * sqrt_f(stiffness * mass)))
    , m_velocity(initial_velocity)
{
}

Spring Spring::with_response(float response_seconds, float bounce, float initial_velocity)
{
    float angular = 2 * pi / response_seconds;
    float stiffness = angular * angular;
    float ratio = 1 - bounce;
    return Spring(1, stiffness, 2 * ratio * angular, initial_velocity);
}

// x(t) is how far from rest it is, starting at -1 with velocity m_velocity; the
// result is 1 + x(t).
Spring Spring::with_frequency(float natural_frequency, float damping_ratio, float initial_velocity)
{
    return Spring(1, natural_frequency * natural_frequency, 2 * damping_ratio * natural_frequency, initial_velocity);
}

float Spring::value_at(float seconds) const
{
    if (!(seconds > 0))
        return 0;
    float w = m_natural;
    float z = m_ratio;
    if (z < 0.9999f) {
        float damped = w * sqrt_f(1 - z * z);
        float a = -1;
        float b = (m_velocity - z * w) / damped;
        return 1 + exp_f(-z * w * seconds) * (a * cos_f(damped * seconds) + b * sin_f(damped * seconds));
    }
    if (z > 1.0001f) {
        float root = w * sqrt_f(z * z - 1);
        float r1 = -z * w + root;
        float r2 = -z * w - root;
        // a + b = -1; a r1 + b r2 = velocity.
        float a = (m_velocity + r2) / (r1 - r2);
        float b = -1 - a;
        return 1 + a * exp_f(r1 * seconds) + b * exp_f(r2 * seconds);
    }
    float a = -1;
    float b = m_velocity - w;
    return 1 + (a + b * seconds) * exp_f(-w * seconds);
}

float Spring::velocity_at(float seconds) const
{
    // Numerically: nothing needs more from it than a handoff between animations.
    float step = 0.0005f;
    return (value_at(seconds + step) - value_at(seconds)) / step;
}

float Spring::settling_time(float tolerance) const
{
    // Past the last time it leaves the band: scan in steps a swing can't hide in.
    float step = 1 / (m_natural * 8);
    float last_outside = 0;
    float limit = 30;
    for (float t = 0; t < limit; t += step) {
        float distance = value_at(t) - 1;
        if (distance > tolerance || distance < -tolerance)
            last_outside = t;
        else if (t - last_outside > 6 / m_natural)
            break;
    }
    return last_outside + step;
}

Timing Timing::linear(float seconds)
{
    Timing timing;
    timing.kind = Kind::Linear;
    timing.duration = seconds;
    return timing;
}

Timing Timing::ease_in_out(float seconds)
{
    Timing timing;
    timing.bezier = CubicBezier(0.42f, 0, 0.58f, 1);
    timing.duration = seconds;
    return timing;
}

Timing Timing::ease_out(float seconds)
{
    Timing timing;
    timing.bezier = CubicBezier(0, 0, 0.58f, 1);
    timing.duration = seconds;
    return timing;
}

Timing Timing::ease_in(float seconds)
{
    Timing timing;
    timing.bezier = CubicBezier(0.42f, 0, 1, 1);
    timing.duration = seconds;
    return timing;
}

Timing Timing::spring_with(float response_seconds, float bounce, float initial_velocity)
{
    Timing timing;
    timing.kind = Kind::Spring;
    timing.spring = Spring::with_response(response_seconds, bounce, initial_velocity);
    timing.duration = timing.spring.settling_time();
    return timing;
}

float Timing::value_at(float seconds) const
{
    if (seconds >= duration)
        return 1;
    switch (kind) {
    case Kind::Linear:
        return duration > 0 ? seconds / duration : 1;
    case Kind::Bezier:
        return duration > 0 ? bezier.value_at(seconds / duration) : 1;
    case Kind::Spring:
        return spring.value_at(seconds);
    }
    return 1;
}

}
