/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/StdLibExtras.h>
#include <LibGesture/Recognizer.h>

namespace Gesture {

bool swipe_carries_on(float travel, float speed)
{
    // Going back the other way, quickly, takes it back from anywhere.
    if (speed < -600)
        return false;
    return travel > 0.25f || speed > 600;
}

void Recognizer::emit(Kind kind, float x, float y, float scale)
{
    if (on_event)
        on_event(Event { kind, x, y, scale });
}

float Recognizer::middle_x(bool from_start) const
{
    float total = 0;
    for (auto const& finger : m_fingers) {
        if (finger.down)
            total += from_start ? finger.start_x : finger.x;
    }
    return m_count ? total / static_cast<float>(m_count) : 0;
}

float Recognizer::middle_y(bool from_start) const
{
    float total = 0;
    for (auto const& finger : m_fingers) {
        if (finger.down)
            total += from_start ? finger.start_y : finger.y;
    }
    return m_count ? total / static_cast<float>(m_count) : 0;
}

float Recognizer::spread(bool from_start) const
{
    float const mx = middle_x(from_start);
    float const my = middle_y(from_start);
    float total = 0;
    for (auto const& finger : m_fingers) {
        if (!finger.down)
            continue;
        float const dx = (from_start ? finger.start_x : finger.x) - mx;
        float const dy = (from_start ? finger.start_y : finger.y) - my;
        total += static_cast<float>(Base::Math::square_root(static_cast<double>(dx * dx + dy * dy)));
    }
    return m_count ? total / static_cast<float>(m_count) : 0;
}

void Recognizer::start_over(double now)
{
    for (auto& finger : m_fingers) {
        finger.start_x = finger.x;
        finger.start_y = finger.y;
    }
    m_started = now;
    m_last_time = now;
    m_last_middle_x = middle_x(false);
    m_last_middle_y = middle_y(false);
    m_last_spread = spread(false);
    m_sample_x = m_last_middle_x;
    m_sample_y = m_last_middle_y;
    m_speed_x = 0;
    m_speed_y = 0;
}

void Recognizer::finger_down(int slot, float x, float y, double now)
{
    if (slot < 0 || slot >= most_fingers || m_fingers[slot].down)
        return;
    auto& finger = m_fingers[slot];
    finger = { true, x, y, x, y };
    ++m_count;
    m_most_this_time = max(m_most_this_time, m_count);
    // Another finger, once something's under way, is the end of that: it doesn't turn into something else part way through.
    if (m_state == State::Waiting) {
        start_over(now);
    } else if (m_state == State::Pointing) {
        // From pointing, though, there's nothing to be undone.
        m_state = State::Waiting;
        start_over(now);
    } else {
        if (m_state == State::Scrolling)
            emit(Kind::ScrollEnded, m_speed_x, m_speed_y);
        else if (m_state == State::Swiping)
            emit(Kind::SwipeEnded, m_last_middle_x - middle_x(true), m_speed_x);
        m_state = State::Finished;
    }
}

void Recognizer::decide(double now)
{
    float const across = middle_x(false) - middle_x(true);
    float const down = middle_y(false) - middle_y(true);
    float const distance = static_cast<float>(Base::Math::square_root(static_cast<double>(across * across + down * down)));
    if (m_count == 1) {
        if (distance > slop)
            m_state = State::Pointing;
        return;
    }
    if (m_count == 2) {
        // Fingers going apart or together have a middle that hardly moves.
        float const widened = spread(false) - spread(true);
        if (Base::Math::absolute(widened) > slop && Base::Math::absolute(widened) > distance) {
            m_state = State::Pinching;
            m_last_spread = spread(false);
        } else if (distance > slop) {
            m_state = State::Scrolling;
        }
        return;
    }
    if (m_count >= 3 && distance > slop * 2) {
        if (Base::Math::absolute(across) >= Base::Math::absolute(down)) {
            m_state = State::Swiping;
        } else {
            emit(down < 0 ? Kind::ThreeFingersUp : Kind::ThreeFingersDown);
            m_state = State::Finished;
        }
    }
    (void)now;
}

void Recognizer::finger_moved(int slot, float x, float y, double now)
{
    if (slot < 0 || slot >= most_fingers || !m_fingers[slot].down)
        return;
    m_fingers[slot].x = x;
    m_fingers[slot].y = y;
    if (m_state == State::Waiting)
        decide(now);

    float const mx = middle_x(false);
    float const my = middle_y(false);
    float const step_x = mx - m_last_middle_x;
    float const step_y = my - m_last_middle_y;
    double const since = now - m_last_time;
    if (since > 0.0005) {
        // Mostly how fast it's going now, and some of how it has been. From one time to the next, and not from one finger to the
        // next: they all report in the same moment, and the first of them is only part of how far it went.
        float const seconds = static_cast<float>(since);
        m_speed_x = m_speed_x * 0.4f + (mx - m_sample_x) / seconds * 0.6f;
        m_speed_y = m_speed_y * 0.4f + (my - m_sample_y) / seconds * 0.6f;
        m_last_time = now;
        m_sample_x = mx;
        m_sample_y = my;
    }
    switch (m_state) {
    case State::Pointing:
        emit(Kind::PointerMoved, x, y);
        break;
    case State::Scrolling:
        if (step_x != 0 || step_y != 0)
            emit(Kind::Scrolled, step_x, step_y);
        break;
    case State::Pinching: {
        float const now_spread = spread(false);
        if (m_last_spread > 0 && now_spread > 0 && now_spread != m_last_spread)
            emit(Kind::Pinched, 0, 0, now_spread / m_last_spread);
        m_last_spread = now_spread;
        break;
    }
    case State::Swiping:
        emit(Kind::SwipeChanged, mx - middle_x(true), m_speed_x);
        break;
    default:
        break;
    }
    m_last_middle_x = mx;
    m_last_middle_y = my;
}

void Recognizer::finger_up(int slot, double now)
{
    if (slot < 0 || slot >= most_fingers || !m_fingers[slot].down)
        return;
    bool const quick = now - m_started <= tap_seconds;
    // What it was while all of them were there.
    float const mx = middle_x(false);
    float const my = middle_y(false);
    float const across = mx - middle_x(true);
    if (m_state == State::Scrolling)
        emit(Kind::ScrollEnded, m_speed_x, m_speed_y);
    else if (m_state == State::Swiping)
        emit(Kind::SwipeEnded, across, m_speed_x);
    else if (m_state == State::Waiting && quick && m_most_this_time == 1)
        emit(Kind::Tap, mx, my);
    else if (m_state == State::Waiting && quick && m_most_this_time == 2)
        emit(Kind::SecondaryTap, mx, my);
    if (m_state != State::Pointing)
        m_state = State::Finished;
    m_fingers[slot].down = false;
    --m_count;
    if (m_count == 0) {
        m_state = State::Waiting;
        m_most_this_time = 0;
    } else if (m_state == State::Pointing) {
        // Still one there, or more: where that one is doesn't jump.
        start_over(now);
    }
}

}
