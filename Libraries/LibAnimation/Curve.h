/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Animation {

// CSS-style timing: y over x from (0, 0) to (1, 1), through two control points.
class CubicBezier {
public:
    constexpr CubicBezier(float x1, float y1, float x2, float y2)
        : m_x1(x1)
        , m_y1(y1)
        , m_x2(x2)
        , m_y2(y2)
    {
    }

    // For progress x (0 to 1) along time.
    [[nodiscard]] float value_at(float x) const;

private:
    float m_x1, m_y1, m_x2, m_y2;
};

// A mass on a spring, let go at 0 and settling at 1, solved exactly: however long a
// frame takes, it lands where the physics says.
class Spring {
public:
    // Velocity in units (of the whole travel) per second.
    Spring(float mass, float stiffness, float damping, float initial_velocity = 0);
    // As people state them: how long a swing takes, and how much it bounces
    // (0 none, up to 1 for ever). As SwiftUI's spring(duration:bounce:).
    [[nodiscard]] static Spring with_response(float response_seconds, float bounce, float initial_velocity = 0);

    [[nodiscard]] float value_at(float seconds) const;
    [[nodiscard]] float velocity_at(float seconds) const;
    // When it stays within `tolerance` of 1 from then on.
    [[nodiscard]] float settling_time(float tolerance = 0.001f) const;

private:
    float m_natural;     // Undamped angular frequency.
    float m_ratio;       // Damping ratio.
    float m_velocity;    // At the start.
};

// How a value gets from one end to the other.
struct Timing {
    enum class Kind : u8 {
        Linear,
        Bezier,
        Spring,
    };

    Kind kind { Kind::Bezier };
    CubicBezier bezier { 0.25f, 0.1f, 0.25f, 1 };
    Spring spring { 1, 100, 20 };
    // In seconds. A spring's own settling time, if none is given.
    float duration { 0.25f };

    [[nodiscard]] static Timing linear(float seconds);
    [[nodiscard]] static Timing ease_in_out(float seconds = 0.25f);
    [[nodiscard]] static Timing ease_out(float seconds = 0.25f);
    [[nodiscard]] static Timing ease_in(float seconds = 0.25f);
    [[nodiscard]] static Timing spring_with(float response_seconds, float bounce, float initial_velocity = 0);

    // Progress at this time since the start: 0 and 1 at the ends, though a spring
    // goes past them on its way.
    [[nodiscard]] float value_at(float seconds) const;
};

}
