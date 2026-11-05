/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/Types.h>

// What fingers mean. Only that: given where they go and when, it says what's been done, with no idea what's to happen because of it,
// or what kind of thing they're on. Which is what lets it be tested on the host, where there are no fingers.
//
// One finger moves the pointer, and taps. Two scroll, pinch, and tap for the other button. Three swipe between Spaces, and go up
// for Overview and down to leave it.

namespace Gesture {

enum class Kind : u8 {
    PointerMoved,   // x, y: where to.
    Tap,            // x, y: where.
    SecondaryTap,   // x, y: where.
    Scrolled,       // x, y: how far the fingers went since the last, in points.
    ScrollEnded,    // x, y: how fast they were going, in points a second.
    Pinched,        // scale: how much bigger or smaller, since the last.
    SwipeChanged,   // x: how far across, in all, since it began; y is how fast, in points a second.
    SwipeEnded,     // The same, for the last time.
    ThreeFingersUp,
    ThreeFingersDown,
};

struct Event {
    Kind kind { Kind::PointerMoved };
    float x { 0 };
    float y { 0 };
    float scale { 1 };
};

// Let go part way between two Spaces, whether it goes on to the next or back to where it was: `travel` is how far it had got (0 to 1), and
// `speed` how fast it was going that way, in points a second. A quarter of the way, or a flick.
[[nodiscard]] bool swipe_carries_on(float travel, float speed);

class Recognizer {
public:
    Function<void(Event const&)> on_event;

    // Positions in points. `now` in seconds, always going up. A slot is a finger for as long as it stays down.
    void finger_down(int slot, float x, float y, double now);
    void finger_moved(int slot, float x, float y, double now);
    void finger_up(int slot, double now);

    [[nodiscard]] int fingers_down() const { return m_count; }

private:
    static constexpr int most_fingers = 10;
    // How far they can go and it still be a tap, and how long it can take.
    static constexpr float slop = 8;
    static constexpr double tap_seconds = 0.3;

    enum class State : u8 {
        Waiting,       // What it's going to be hasn't been said.
        Pointing,      // One finger, moving.
        Scrolling,
        Pinching,
        Swiping,
        Finished,      // Done with, whatever else happens until they've all gone.
    };

    struct Finger {
        bool down { false };
        float x { 0 };
        float y { 0 };
        float start_x { 0 };
        float start_y { 0 };
    };

    void emit(Kind, float x = 0, float y = 0, float scale = 1);
    void start_over(double now);
    void decide(double now);
    [[nodiscard]] float middle_x(bool from_start) const;
    [[nodiscard]] float middle_y(bool from_start) const;
    [[nodiscard]] float spread(bool from_start) const;

    Finger m_fingers[most_fingers];
    int m_count { 0 };
    int m_most_this_time { 0 };
    State m_state { State::Waiting };
    double m_started { 0 };
    double m_last_time { 0 };
    float m_last_middle_x { 0 };
    float m_last_middle_y { 0 };
    float m_last_spread { 0 };
    float m_sample_x { 0 };
    float m_sample_y { 0 };
    float m_speed_x { 0 };
    float m_speed_y { 0 };
};

}
